"""Prepare authenticated database connections only after Gunicorn forks a worker."""


def post_worker_init(worker):
    from app import warm_pool
    warm_pool()


def worker_exit(server, worker):
    from app import close_pool
    close_pool()
