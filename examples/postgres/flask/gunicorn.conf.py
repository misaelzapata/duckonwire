"""Prepare authenticated database connections only after Gunicorn forks a worker."""
import logging
import threading

from psycopg_pool import PoolClosed

POOL_CHECK_INTERVAL_SECONDS = 60


def _maintain_pool(pool, stop):
    # The native protected endpoint closes sockets after five idle minutes.
    # The official check only leases idle sessions, then returns or replaces them.
    while not stop.wait(POOL_CHECK_INTERVAL_SECONDS):
        if pool.closed:
            return
        try:
            pool.check()
        except PoolClosed:
            return
        except Exception as error:
            logging.getLogger(__name__).warning("Pool health check failed (%s)", type(error).__name__)


def post_worker_init(worker):
    from app import connection_pool, warm_pool
    current = getattr(worker, "_pool_health_thread", None)
    if current is not None and current.is_alive():
        return
    warm_pool()
    stop = threading.Event()
    thread = threading.Thread(target=_maintain_pool, args=(connection_pool(), stop),
                              name="pool-health", daemon=True)
    worker._pool_health_stop = stop
    worker._pool_health_thread = thread
    thread.start()


def worker_exit(server, worker):
    from app import close_pool
    stop = getattr(worker, "_pool_health_stop", None)
    thread = getattr(worker, "_pool_health_thread", None)
    if stop is not None:
        stop.set()
    if thread is not None:
        thread.join(timeout=3)
    close_pool()
    if thread is not None and thread.is_alive():
        thread.join(timeout=3)
        if thread.is_alive():
            logging.getLogger(__name__).error("Pool health check did not finish before worker exit")
