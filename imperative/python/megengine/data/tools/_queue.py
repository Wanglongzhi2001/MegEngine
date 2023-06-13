# -*- coding: utf-8 -*-
import binascii
import os
import pickle
import queue
import subprocess
from multiprocessing import Queue

import pyarrow
import pyarrow.plasma as plasma

from ...logger import get_logger

logger = get_logger(__name__)

# Each process only need to start one plasma store, so we set it as a global variable.
# TODO: how to share between different processes?
MGE_PLASMA_STORE_MANAGER = None


def _clear_plasma_store():
    # `_PlasmaStoreManager.__del__` will not be called automaticly in subprocess,
    # so this function should be called explicitly
    global MGE_PLASMA_STORE_MANAGER
    if MGE_PLASMA_STORE_MANAGER is not None and MGE_PLASMA_STORE_MANAGER.refcount == 0:
        del MGE_PLASMA_STORE_MANAGER
        MGE_PLASMA_STORE_MANAGER = None


class _ExceptionWrapper:
    def __init__(self, exc_type=None, exc_msg=None, where="in background"):
        self.exc_type = exc_type
        self.exc_msg = exc_msg
        self.where = where

    def reraise(self):
        if self.exc_type == "PlasmaStoreFull" or self.exc_type == queue.Full:
            msg = "Caught {} {}.\nSolution: {}".format(
                self.exc_type,
                self.where,
                'Because of PlasmaStore out of memory, so you can fix it by using: os.environ["MGE_PLASMA_MEMORY"] = x000000000(4GB for default) or reduce the number-workers',
            )
            raise RuntimeError(msg)
        elif self.exc_type == "ObjectIDGetError":
            logger.warning(
                "ObjectID not found in plasma store, some data will be lost, try to reduce the number-workers for debugging!"
            )
            return
        else:
            msg = "Caught {} {}.\nOriginal {}".format(
                self.exc_type, self.where, self.exc_msg
            )
        raise RuntimeError(msg)

    @staticmethod
    def _serialize_Exception(val):
        return {"exc_type": val.exc_type, "exc_msg": val.exc_msg, "where": val.where}

    @staticmethod
    def _deserialize_Exception(data):
        return _ExceptionWrapper(data["exc_type"], data["exc_msg"], data["where"])


class _PlasmaStoreManager:
    __initialized = False

    def __init__(self):
        self.socket_name = "/tmp/mge_plasma_{}".format(
            binascii.hexlify(os.urandom(8)).decode()
        )
        debug_flag = bool(os.environ.get("MGE_DATALOADER_PLASMA_DEBUG", 0))
        MGE_PLASMA_MEMORY = int(
            os.environ.get("MGE_PLASMA_MEMORY", 4 * 1024 * 1024 * 1024)
        )  # 4GB
        # NOTE: this is a hack. Directly use `plasma_store` may make subprocess
        # difficult to handle the exception happened in `plasma-store-server`.
        # For `plasma_store` is just a wrapper of `plasma-store-server`, which use
        # `os.execv` to call the executable `plasma-store-server`.
        cmd_path = os.path.join(pyarrow.__path__[0], "plasma-store-server")
        self.plasma_store = subprocess.Popen(
            [cmd_path, "-s", self.socket_name, "-m", str(MGE_PLASMA_MEMORY),],
            stdout=None if debug_flag else subprocess.DEVNULL,
            stderr=None if debug_flag else subprocess.DEVNULL,
        )
        self.__initialized = True
        self.refcount = 1

    def __del__(self):
        if self.__initialized and self.plasma_store.returncode is None:
            self.plasma_store.kill()


class PlasmaShmQueue:
    def __init__(self, maxsize: int = 0):
        r"""Use pyarrow in-memory plasma store to implement shared memory queue.
        Compared to native `multiprocess.Queue`, `PlasmaShmQueue` avoid pickle/unpickle
        and communication overhead, leading to better performance in multi-process
        application.

        Args:
            maxsize: maximum size of the queue, `None` means no limit. (default: ``None``)
        """

        # Lazy start the plasma store manager
        global MGE_PLASMA_STORE_MANAGER
        if MGE_PLASMA_STORE_MANAGER is None:
            try:
                MGE_PLASMA_STORE_MANAGER = _PlasmaStoreManager()
            except Exception as e:
                err_info = (
                    "Please make sure pyarrow installed correctly!\n"
                    "You can try reinstall pyarrow and see if you can run "
                    "`plasma_store -s /tmp/mge_plasma_xxx -m 1000` normally."
                )
                raise RuntimeError(
                    "Exception happened in starting plasma_store: {}\n"
                    "Tips: {}".format(str(e), err_info)
                )
        else:
            MGE_PLASMA_STORE_MANAGER.refcount += 1

        self.socket_name = MGE_PLASMA_STORE_MANAGER.socket_name

        # TODO: how to catch the exception happened in `plasma.connect`?
        self.client = None

        # Used to store the header for the data.(ObjectIDs)
        self.queue = Queue(maxsize)  # type: Queue

    def get_error(self, exc_type, where="in background"):
        data = _ExceptionWrapper(exc_type=exc_type, where=where)
        data_buffer = pickle.dumps(data)
        return data_buffer

    def put(self, data, block=True, timeout=None):
        if self.client is None:
            self.client = plasma.connect(self.socket_name)
        try:
            object_id = self.client.put(data)
        except plasma.PlasmaStoreFull:
            where = "in DataLoader Plasma Store Put Full"
            exc_type = "PlasmaStoreFull"
            error = self.get_error(exc_type, where)
            object_id = self.client.put((error,))

        try:
            self.queue.put(object_id, block, timeout)
        except queue.Full:
            self.client.delete([object_id])
            raise queue.Full

    def get(self, block=True, timeout=None):
        if self.client is None:
            self.client = plasma.connect(self.socket_name)
        object_id = self.queue.get(block, timeout)
        if not self.client.contains(object_id):
            exc_type = "ObjectIDGetError"
            data = (self.get_error(exc_type),)
        else:
            data = self.client.get(object_id)
            self.client.delete([object_id])
        return data

    def qsize(self):
        return self.queue.qsize()

    def empty(self):
        return self.queue.empty()

    def join(self):
        self.queue.join()

    def disconnect_client(self):
        if self.client is not None:
            self.client.disconnect()

    def close(self):
        self.queue.close()
        self.disconnect_client()
        global MGE_PLASMA_STORE_MANAGER
        MGE_PLASMA_STORE_MANAGER.refcount -= 1
        _clear_plasma_store()

    def cancel_join_thread(self):
        self.queue.cancel_join_thread()
