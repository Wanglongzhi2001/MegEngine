import logging
import os
import platform as py_platform
import threading
import warnings
from functools import lru_cache, partial
from typing import Any, Dict, List, Optional, Union

import numpy as np
from jaxlib import xla_client

from ..lib import cuda_path
from .config import bool_env, config, flags, int_env

XlaBackend = xla_client._xla.Client

ShardedBuffer = Any

FLAGS = flags.FLAGS

logger = logging.getLogger(__name__)

flags.DEFINE_string(
    "jax_xla_backend", "", "Deprecated, please use --jax_platforms instead."
)
flags.DEFINE_string(
    "jax_backend_target",
    os.getenv("JAX_BACKEND_TARGET", "").lower(),
    'Either "local" or "rpc:address" to connect to a remote service target.',
)
# TODO: warn when this is used once we test out --jax_platforms a bit
flags.DEFINE_string(
    "jax_platform_name",
    os.getenv("JAX_PLATFORM_NAME", "").lower(),
    "Deprecated, please use --jax_platforms instead.",
)
flags.DEFINE_bool(
    "jax_disable_most_optimizations",
    bool_env("JAX_DISABLE_MOST_OPTIMIZATIONS", False),
    "Try not to do much optimization work. This can be useful if the cost of "
    "optimization is greater than that of running a less-optimized program.",
)
flags.DEFINE_integer(
    "jax_xla_profile_version",
    int_env("JAX_XLA_PROFILE_VERSION", 0),
    "Optional profile version for XLA compilation. "
    "This is meaningful only when XLA is configured to "
    "support the remote compilation profile feature.",
)
flags.DEFINE_string(
    "jax_cuda_visible_devices",
    "all",
    'Restricts the set of CUDA devices that JAX will use. Either "all", or a '
    "comma-separate list of integer device IDs.",
)
flags.DEFINE_string(
    "jax_rocm_visible_devices",
    "all",
    'Restricts the set of ROCM devices that JAX will use. Either "all", or a '
    "comma-separate list of integer device IDs.",
)


def get_compile_options(
    num_replicas: int,
    num_partitions: int,
    device_assignment=None,
    use_spmd_partitioning: bool = True,
    use_auto_spmd_partitioning: bool = False,
    auto_spmd_partitioning_mesh_shape=[],
    auto_spmd_partitioning_mesh_ids=[],
) -> xla_client.CompileOptions:
    """Returns the compile options to use, as derived from flag values.

    Args:
        num_replicas: Number of replicas for which to compile.
        num_partitions: Number of partitions for which to compile.
        device_assignment: Optional ndarray of jax devices indicating the assignment
        of logical replicas to physical devices (default inherited from
        xla_client.CompileOptions). Must be consistent with `num_replicas` and
        `num_partitions`.
        use_spmd_partitioning: boolean indicating whether to enable SPMD or MPMD
        partitioning in XLA.
        use_auto_spmd_partitioning: boolean indicating whether to automatically
        generate XLA shardings for SPMD partitioner.
        auto_spmd_partitioning_mesh_shape: device mesh shape used to create
        auto_spmd_partitioning search space.
        auto_spmd_partitioning_mesh_ids: device ids used to create
        auto_spmd_partitioning search space.
    """
    compile_options = xla_client.CompileOptions()
    compile_options.num_replicas = num_replicas
    compile_options.num_partitions = num_partitions
    build_options = compile_options.executable_build_options
    build_options.use_spmd_partitioning = use_spmd_partitioning
    build_options.use_auto_spmd_partitioning = use_auto_spmd_partitioning
    if use_auto_spmd_partitioning:
        build_options.auto_spmd_partitioning_mesh_shape = (
            auto_spmd_partitioning_mesh_shape
        )
        build_options.auto_spmd_partitioning_mesh_ids = auto_spmd_partitioning_mesh_ids
    if device_assignment is not None:
        logger.debug(
            "get_compile_options: num_replicas=%s num_partitions=%s device_assignment=%s",
            num_replicas,
            num_partitions,
            device_assignment,
        )
        device_assignment = np.array(device_assignment)

        # Allow 1D device assignment if num_partitions is 1.
        if (device_assignment.ndim == 1) and (num_partitions == 1):
            device_assignment = device_assignment[:, None]

        if num_replicas != device_assignment.shape[0]:
            msg = "device_assignment does not match num_replicas: {} vs {}."
            raise ValueError(msg.format(device_assignment, num_replicas))

        if num_partitions != device_assignment.shape[1]:
            msg = "device_assignment does not match num_partitions: {} vs {}."
            raise ValueError(msg.format(device_assignment, num_partitions))

        if device_assignment.dtype == object:
            device_assignment = np.vectorize(lambda d: d.id, otypes=[int])(
                device_assignment
            )
        device_assignment = xla_client.DeviceAssignment.create(device_assignment)
        assert device_assignment.replica_count() == num_replicas
        assert device_assignment.computation_count() == num_partitions
        compile_options.device_assignment = device_assignment

    debug_options = compile_options.executable_build_options.debug_options
    if cuda_path is not None:
        debug_options.xla_gpu_cuda_data_dir = cuda_path

    if FLAGS.jax_disable_most_optimizations:
        debug_options.xla_backend_optimization_level = 0
        debug_options.xla_llvm_disable_expensive_passes = True
        debug_options.xla_test_all_input_layouts = False

    compile_options.profile_version = FLAGS.jax_xla_profile_version
    return compile_options


# Backends, in increasing order of preference.
# We have no particular opinion about how "backends" relate to "devices". For
# example, there could be multiple backends that provide the same kind of
# device.
_backend_factories = {}
_default_backend = None
_backends: Dict[str, Any] = {}
_backends_errors: Dict[str, str] = {}
_backend_lock = threading.Lock()


def register_backend_factory(name, factory, *, priority=0):
    with _backend_lock:
        if name in _backends:
            raise RuntimeError(f"Backend {name} already initialized")
    _backend_factories[name] = (factory, priority)


register_backend_factory(
    "interpreter", xla_client.make_interpreter_client, priority=-100
)
register_backend_factory(
    "cpu", partial(xla_client.make_cpu_client, use_tfrt=True), priority=0
)


def make_gpu_client(*, platform_name, visible_devices_flag):
    from ..distribute import global_state

    visible_devices = global_state.visible_devices
    if visible_devices != "all":
        allowed_devices = {int(x) for x in visible_devices.split(",")}
    else:
        allowed_devices = None
    return xla_client.make_gpu_client(
        distributed_client=global_state.client,
        node_id=global_state.process_id,
        platform_name=platform_name,
        allowed_devices=allowed_devices,
    )


if hasattr(xla_client, "make_gpu_client"):
    register_backend_factory(
        "cuda",
        partial(
            make_gpu_client,
            platform_name="cuda",
            visible_devices_flag="jax_cuda_visible_devices",
        ),
        priority=200,
    )
    register_backend_factory(
        "rocm",
        partial(
            make_gpu_client,
            platform_name="rocm",
            visible_devices_flag="jax_rocm_visible_devices",
        ),
        priority=200,
    )

if hasattr(xla_client, "make_plugin_device_client"):
    # It is assumed that if jax has been built with a plugin client, then the
    # user wants to use the plugin client by default. Therefore, it gets the
    # highest priority.
    register_backend_factory(
        "plugin", xla_client.make_plugin_device_client, priority=400
    )

_platform_aliases = {
    "cuda": "gpu",
    "rocm": "gpu",
}

_alias_to_platforms: Dict[str, List[str]] = {}
for _platform, _alias in _platform_aliases.items():
    _alias_to_platforms.setdefault(_alias, []).append(_platform)


def is_known_platform(platform: str):
    # A platform is valid if there is a registered factory for it. It does not
    # matter if we were unable to initialize that platform; we only care that
    # we've heard of it and it isn't, e.g., a typo.
    return platform in _backend_factories.keys() or platform in _platform_aliases.keys()


def canonicalize_platform(platform: str) -> str:
    """Replaces platform aliases with their concrete equivalent.

    In particular, replaces "gpu" with either "cuda" or "rocm", depending on which
    hardware is actually present. We want to distinguish "cuda" and "rocm" for
    purposes such as MLIR lowering rules, but in many cases we don't want to
    force users to care.
    """
    platforms = _alias_to_platforms.get(platform, None)
    if platforms is None:
        return platform

    b = backends()
    for p in platforms:
        if p in b.keys():
            return p
    raise RuntimeError(
        f"Unknown backend: '{platform}' requested, but no "
        f"platforms that are instances of {platform} are present. "
        "Platforms present are: " + ",".join(b.keys())
    )


def expand_platform_alias(platform: str) -> List[str]:
    """Expands, e.g., "gpu" to ["cuda", "rocm"].

    This is used for convenience reasons: we expect cuda and rocm to act similarly
    in many respects since they share most of the same code.
    """
    return _alias_to_platforms.get(platform, [platform])


def is_gpu(platform):
    return platform in ("cuda", "rocm")


def backends():
    global _backends
    global _backends_errors
    global _default_backend

    with _backend_lock:
        if _backends:
            return _backends
        if config.jax_platforms:
            jax_platforms = config.jax_platforms.split(",")
            platforms = []
            # Allow platform aliases in the list of platforms.
            for platform in jax_platforms:
                platforms.extend(expand_platform_alias(platform))
            priorities = range(len(platforms), 0, -1)
            platforms_and_priorites = zip(platforms, priorities)
        else:
            platforms_and_priorites = (
                (platform, priority)
                for platform, (_, priority) in _backend_factories.items()
            )
        default_priority = -1000
        if hasattr(xla_client, "maybe_load_pjrt_plugins"):
            xla_client.maybe_load_pjrt_plugins()
        for platform, priority in platforms_and_priorites:
            try:
                backend = _init_backend(platform)
                _backends[platform] = backend

                if priority > default_priority:
                    _default_backend = backend
                    default_priority = priority
            except Exception as err:
                if platform in ("cpu", "interpreter"):
                    # We always expect the CPU and interpreter backends to initialize
                    # successfully.
                    raise
                else:
                    # If the backend isn't built into the binary, or if it has no devices,
                    # we expect a RuntimeError.
                    err_msg = f"Unable to initialize backend '{platform}': {err}"
                    if config.jax_platforms:
                        err_msg += " (set JAX_PLATFORMS='' to automatically choose an available backend)"
                        raise RuntimeError(err_msg)
                    else:
                        _backends_errors[platform] = str(err)
                        logger.info(err_msg)
                        continue
        # We don't warn about falling back to CPU on Mac OS, because we don't
        # support anything else there at the moment and warning would be pointless.
        if (
            py_platform.system() != "Darwin"
            and _default_backend.platform == "cpu"
            and FLAGS.jax_platform_name != "cpu"
        ):
            logger.warning(
                "No GPU/TPU found, falling back to CPU. "
                "(Set TF_CPP_MIN_LOG_LEVEL=0 and rerun for more info.)"
            )
        return _backends


def _clear_backends():
    global _backends
    global _backends_errors
    global _default_backend

    logger.info("Clearing JAX backend caches.")
    with _backend_lock:
        _backends = {}
        _backends_errors = {}
        _default_backend = None

    get_backend.cache_clear()


def _init_backend(platform):
    factory, unused_priority = _backend_factories.get(platform, (None, None))
    if factory is None:
        raise RuntimeError(f"Unknown backend '{platform}'")

    logger.debug("Initializing backend '%s'", platform)
    backend = factory()

    if backend is None:
        raise RuntimeError(f"Could not initialize backend '{platform}'")
    if backend.device_count() == 0:
        raise RuntimeError(f"Backend '{platform}' provides no devices.")
    logger.debug("Backend '%s' initialized", platform)
    return backend


def _get_backend_uncached(platform=None):
    if not isinstance(platform, (type(None), str)):
        return platform

    platform = platform or FLAGS.jax_xla_backend or FLAGS.jax_platform_name or None

    bs = backends()
    if platform is not None:
        platform = canonicalize_platform(platform)
        backend = bs.get(platform, None)
        if backend is None:
            if platform in _backends_errors:
                raise RuntimeError(
                    f"Backend '{platform}' failed to initialize: "
                    f"{_backends_errors[platform]}"
                )
            raise RuntimeError(f"Unknown backend {platform}")
        return backend
    else:
        return _default_backend


@lru_cache(maxsize=None)  # don't use util.memoize because there is no X64 dependence.
def get_backend(platform=None):
    return _get_backend_uncached(platform)


def get_device_backend(device=None):
    """Returns the Backend associated with `device`, or the default Backend."""
    if device is not None:
        return device.client
    return get_backend()


def device_count(backend: Optional[Union[str, XlaBackend]] = None) -> int:
    """Returns the total number of devices.

    On most platforms, this is the same as :py:func:`jax.local_device_count`.
    However, on multi-process platforms where different devices are associated
    with different processes, this will return the total number of devices across
    all processes.

    Args:
        backend: This is an experimental feature and the API is likely to change.
        Optional, a string representing the xla backend: ``'cpu'``, ``'gpu'``, or
        ``'tpu'``.

    Returns:
        Number of devices.

    """
    return int(get_backend(backend).device_count())


def local_device_count(backend: Optional[Union[str, XlaBackend]] = None) -> int:
    """Returns the number of devices addressable by this process."""
    return int(get_backend(backend).local_device_count())


def devices(
    backend: Optional[Union[str, XlaBackend]] = None
) -> List[xla_client.Device]:
    """Returns a list of all devices for a given backend.

    .. currentmodule:: jaxlib.xla_extension

    Each device is represented by a subclass of :class:`Device` (e.g.
    :class:`CpuDevice`, :class:`GpuDevice`). The length of the returned list is
    equal to ``device_count(backend)``. Local devices can be identified by
    comparing :attr:`Device.process_index` to the value returned by
    :py:func:`jax.process_index`.

    If ``backend`` is ``None``, returns all the devices from the default backend.
    The default backend is generally ``'gpu'`` or ``'tpu'`` if available,
    otherwise ``'cpu'``.

    Args:
        backend: This is an experimental feature and the API is likely to change.
        Optional, a string representing the xla backend: ``'cpu'``, ``'gpu'``, or
        ``'tpu'``.

    Returns:
        List of Device subclasses.
    """
    return get_backend(backend).devices()


def default_backend() -> str:
    """Returns the platform name of the default XLA backend."""
    return get_backend(None).platform


def local_devices(
    process_index: Optional[int] = None,
    backend: Optional[Union[str, XlaBackend]] = None,
    host_id: Optional[int] = None,
) -> List[xla_client.Device]:
    """Like :py:func:`jax.devices`, but only returns devices local to a given process.

    If ``process_index`` is ``None``, returns devices local to this process.

    Args:
        process_index: the integer index of the process. Process indices can be
        retrieved via ``len(jax.process_count())``.
        backend: This is an experimental feature and the API is likely to change.
        Optional, a string representing the xla backend: ``'cpu'``, ``'gpu'``, or
        ``'tpu'``.

    Returns:
        List of Device subclasses.
    """
    if host_id is not None:
        warnings.warn(
            "The argument to jax.local_devices has been renamed from `host_id` to "
            "`process_index`. This alias will eventually be removed; please update "
            "your code."
        )
        process_index = host_id
    if process_index is None:
        process_index = get_backend(backend).process_index()
    if not (0 <= process_index < process_count()):
        raise ValueError(f"Unknown process_index {process_index}")
    return [d for d in devices(backend) if d.process_index == process_index]


def process_index(backend: Optional[Union[str, XlaBackend]] = None) -> int:
    """Returns the integer process index of this process.

    On most platforms, this will always be 0. This will vary on multi-process
    platforms though.

    Args:
        backend: This is an experimental feature and the API is likely to change.
        Optional, a string representing the xla backend: ``'cpu'``, ``'gpu'``, or
        ``'tpu'``.

    Returns:
        Integer process index.
    """
    return get_backend(backend).process_index()

# returns the number of mge processes associated with the backend
def process_count(backend: Optional[Union[str, XlaBackend]] = None) -> int:
    return max(d.process_index for d in devices(backend)) + 1
