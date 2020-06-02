# Homa DPDK implementation with Aeolus support
This project implements Homa (sigcomm'18) transport protocol with Aeolus (sigcomm'20) support.

## File description
- ```dpdk-18.05/app/test-pmd/flowgen.c```
This file contains the major Homa implementation code with Aeolus support.

- ```dpdk-18.05/app/test-pmd/config/```
This folder contains the configuration files (e.g., server ip/eth address, traffic demand). We use the ```dpdk-18.05/app/test-pmd/config/flow_file_generator/flow_file_generator.c``` script to generate most of the traffic demand. 

- ```dpdk-18.05/app/test-pmd/result/```
This folder contains the raw evaluation results of the Aeolus (sigcomm'20) paper.

## System requirement
Servers should run in Linux system with NICs supporting DPDK. Switches should support RED/ECN so as to enable Aeolus selective dropping. For example, the experiment setup in the Aeolus (sigcomm'20) paper: 
* 8 HUAWEI Tecal RH1288 V2 servers.
* 1 Mellanox SN2100 switch.
* All servers are connected to the switch with Intel 82599EB 10GbE NIC.

## How to run
Step 1: Download the source code and build the DPDK, following instructions in [DPDK - Getting Started Guide for Linux](http://doc.dpdk.org/guides/linux_gsg/build_dpdk.html).

Step 2: Setup configuration files in ```dpdk-18.05/app/test-pmd/config/```
* Fill in the eth address in file ```app/test-pmd/config/eth_addr_info.txt```
* Fill in the ip address in file ```app/test-pmd/config/ip_addr_info.txt```
* Generate the traffic demand with ```app/test-pmd/config/flow_file_generator/flow_file_generator```. We already generated files that can be directly used to reproduce the Aeolus (sigcomm'20) paper results.

Note: The ```eth_addr``` and ```ip_addr``` should follow the same order, i.e., i-th ```ip_addr``` and i-th ```eth_addr``` should refer to the same server.

Step 3: Configure the ```flowgen.c``` file in all servers,

Option 1: Configuration to enable aeolus,
```
#define ENABLE_AEOLUS 1
```

Option 2: Configuration to disable aeolus,
```
#define ENABLE_AEOLUS 0
```

Other configurations (set the flow_filename accordingly, e.g., based on your flow size, and leave the others as default):
```
/* Configuration files to be placed in app/test-pmd/config/ */
/* The first line (server_id=0) is used for warm-up receiver */
static const char ethaddr_filename[] = "app/test-pmd/config/eth_addr_info.txt";
static const char ipaddr_filename[] = "app/test-pmd/config/ip_addr_info.txt";
/* The first few lines are used for warm-up flows */
static const char flow_filename[] = "app/test-pmd/config/flow_info_incast.txt";
```

Finally, set ```this_server_id``` as ```i``` if the current server is the ```i```-th ip in the ```ip_addr_info.txt``` file.

Step 4: Rebuild testpmd following instructions in [Testpmd Application User Guide](https://doc.dpdk.org/guides/testpmd_app_ug/build_app.html).

Step 5: Run testpmd with the following command in all servers. Servers will first sync and warm-up with each other and then run flows if configured correctly. You can use this tool [Run-Cluster](https://github.com/gaoxiongzeng/Run-Cluster) to run commands simultaneously on multiple servers.

```sudo ./testpmd -l 0-3 -n 4 -- --forward-mode=flowgen --portmask=0x1```

Step 6: Collect results from each participating server. Server will print out its send flow fct and receive flow fct after finished running testpmd. Read ```print_fct()``` in ```flowgen.c``` for more detailed print options.

## Contact
If you have any question about this project, please contact [Gaoxiong Zeng](http://gaoxiongzeng.github.io/).

## References
- [Homa DPDK and Aeolus (SIGCOMM'20)](https://github.com/gaoxiongzeng/dpdk-18.05).
