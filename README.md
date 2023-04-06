# RoCE-Pingpong

## How to use RoCE-Pingpong

#### Prerequisites

- Ensure that rdma-core is installed to enable support for Soft-RoCE (on Ubuntu run **_sudo apt-get install rdma-core_** if it is not yet installed)
- If Soft-RoCE is not yet set up, please refer to this guide: **_https://github.com/linux-rdma/rdma-core/blob/master/Documentation/rxe.md_**
- In case they are not yet installed, install the libibverbs and librdmacm libraries (use **_sudo apt-get install libibverbs-dev_** and **_sudo apt-get install librdmacm-dev_** on Ubuntu)
- For the compilation of the source code the GNU Compiler Collection **_gcc_** also needs to be installed
- _optional_: To check the status and get detailed information on the RDMA devices the libibverbs-utils library can optionally be installed to add support for commands like _ibv_devinfo_ (use **_sudo apt-get install libibverbs-utils_** on Ubuntu)

#### Compilation

- Navigate to the path where the source files are located
- To compile roce_client.c run **_gcc -o roce_client roce_client.c -libverbs -lrdmacm_**
- To compile roce_server.c run **_gcc -o roce_server roce_server.c -libverbs -lrdmacm_**

#### Run RoCE Pingpong

- On the machine that will act as server run **_./roce_server_**
- On the machine that will act as client run **_./roce_client -a "IP Address of server" -s "Message size in bytes" [-p "Port other than RoCE default port 4791" (_optional_)]_**
- Afterwards, the pingpong test will be run and the resulting Write and Read bandwidths will be printed on the shell
- To run the test again, repeat the listed steps again
