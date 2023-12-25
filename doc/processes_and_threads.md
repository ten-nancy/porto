Process and thread dependences
------------------------------

This scheme shows all possible processes and threads in Porto and describes the dependences of creating between them.

![Process and thread dependences](./images/processes_and_threads.svg)

Here is a description of some processes and threads by type:

* **Main Processes**

  * **Master Daemon** creates and holds sockets, starts Server Daemon, and provides its graceful reloading and upgrading.

  * **Server Daemon** handles client requests and kernel events, launches API Request Queues threads to process them, and other work threads.

* **Single Threads**

  * **NetWatchdog** gets and updates some network metrics (from `/proc/net/netstat`) for every container with network, repairs proxy neightbour, synchronizes resolv_conf, etc.

  * **L3StatWatchdog** gets and updates some network metrics (e.g., `net_tx_bytes` and `net_rx_bytes`) for every virtual interface.

  * **StatFsThread** gets and updates filesystem metrics for every volume.

  * **AsyncRemoveThread** removes storage and junk in all active places asynchronously.

* **Workers**

  * **EventWorker** is a pool of workers that is a part of the Server Daemon and handles client requests and kernel events directly.

  * **AsyncUnmounter** is a pool of workers that do umount asynchronously by pinning a mount point with a file descriptor and detaching that mount point.

  * **PinCloser** is a pool of workers and a part of AsyncUnmounter that closes detached mount point pins.

* **API Request Queues** process client requests depending on type and perform all actions with containers, volumes, and other Porto objects.

* **Exeution processes**

  * **Container** prepares all resources for the container and executes the user process, maybe using **TripleFork Process** and **ChildFn**.

  * **PortoInit** waits for the user process exit and handles its exit code.
