import os
import platform
import re
import warnings
from typing import Optional, Tuple

import jaxlib.cpu_feature_guard as cpu_feature_guard
import jaxlib.ducc_fft as ducc_fft
import jaxlib.gpu_linalg as gpu_linalg  # pytype: disable=import-error
import jaxlib.gpu_prng as gpu_prng  # pytype: disable=import-error
import jaxlib.gpu_rnn as gpu_rnn  # pytype: disable=import-error
import jaxlib.gpu_solver as gpu_solver  # pytype: disable=import-error
import jaxlib.gpu_sparse as gpu_sparse  # pytype: disable=import-error
import jaxlib.lapack as lapack
import jaxlib.xla_client as xla_client

try:
    import jaxlib as jaxlib
except ModuleNotFoundError as err:
    raise ModuleNotFoundError(
        "megengine with xla requires jaxlib to be installed."
    ) from err

# some version check code
"""
import jax.version
from jax.version import _minimum_jaxlib_version as _minimum_jaxlib_version_str
try:
  import jaxlib.version
except Exception as err:
  # jaxlib is too old to have version number.
  msg = f'This version of jax requires jaxlib version >= {_minimum_jaxlib_version_str}.'
  raise ImportError(msg) from err


# Checks the jaxlib version before importing anything else from jaxlib.
# Returns the jaxlib version string.
def check_jaxlib_version(jax_version: str, jaxlib_version: str,
                         minimum_jaxlib_version: str):
  # Regex to match a dotted version prefix 0.1.23.456.789 of a PEP440 version.
  # PEP440 allows a number of non-numeric suffixes, which we allow also.
  # We currently do not allow an epoch.
  version_regex = re.compile(r"[0-9]+(?:\.[0-9]+)*")
  def _parse_version(v: str) -> Tuple[int, ...]:
    m = version_regex.match(v)
    if m is None:
      raise ValueError(f"Unable to parse jaxlib version '{v}'")
    return tuple(int(x) for x in m.group(0).split('.'))

  _jax_version = _parse_version(jax_version)
  _minimum_jaxlib_version = _parse_version(minimum_jaxlib_version)
  _jaxlib_version = _parse_version(jaxlib_version)

  if _jaxlib_version < _minimum_jaxlib_version:
    msg = (f'jaxlib is version {jaxlib_version}, but this version '
           f'of jax requires version >= {minimum_jaxlib_version}.')
    raise RuntimeError(msg)

  if _jaxlib_version > _jax_version:
    msg = (f'jaxlib version {jaxlib_version} is newer than and '
           f'incompatible with jax version {jax_version}. Please '
           'update your jax and/or jaxlib packages.')
    raise RuntimeError(msg)

  return _jaxlib_version

version_str = jaxlib.version.__version__
version = check_jaxlib_version(
  jax_version=jax.version.__version__,
  jaxlib_version=jaxlib.version.__version__,
  minimum_jaxlib_version=jax.version._minimum_jaxlib_version)
"""

# Before importing any C compiled modules from jaxlib, first import the CPU
# feature guard module to verify that jaxlib was compiled in a way that only
# uses instructions that are present on this machine.
cpu_feature_guard.check_cpu_features()


xla_extension = xla_client._xla
pytree = xla_client._xla.pytree
jax_jit = xla_client._xla.jax_jit
pmap_lib = xla_client._xla.pmap_lib


# Jaxlib code is split between the Jax and the Tensorflow repositories.
# Only for the internal usage of the JAX developers, we expose a version
# number that can be used to perform changes without breaking the main
# branch on the Jax github.
xla_extension_version = getattr(xla_client, "_version", 0)


# Version number for MLIR:Python APIs, provided by jaxlib.
mlir_api_version = xla_client.mlir_api_version

try:
    from jaxlib import tpu_client as tpu_driver_client  # pytype: disable=import-error
except:
    tpu_driver_client = None  # type: ignore


# TODO: check if we need the same for rocm.
cuda_path: Optional[str]
cuda_path = os.path.join(os.path.dirname(jaxlib.__file__), "cuda")
if not os.path.isdir(cuda_path):
    cuda_path = None

transfer_guard_lib = xla_client._xla.transfer_guard_lib
