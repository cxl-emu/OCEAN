# OCEAN
OCEAN – <ins>O</ins>pen-source <ins>C</ins>XL <ins>E</ins>mulation at Hyperscale <ins>A</ins>rchitecture and <ins>N</ins>etworking

Compute Express Link (CXL) 3.0 introduces powerful memory pooling and promises to transform datacenter architectures. However, the lack of available CXL 3.0 hardware and the complexity of multi-host configurations pose significant challenges to the community. OCEAN is a comprehensive emulation framework that enables full CXL 3.0 functionality, including multi-host memory sharing and pooling support. OCEAN provides emulation of CXL 3.0 features—such as fabric management, dynamic memory allocation, and coherent memory sharing across multiple hosts—in advance of real hardware availability. An evaluation of OCEAN shows that it achieves performance within about 3x of projected native CXL 3.0 speeds having complete compatibility with existing CXL software stacks. We demonstrate the utility of OCEAN through a case study on Genomics Pipeline, distributed database, LLM workloads, observing up to a 15% improvement in application performance compared to traditional RDMA-based approaches.


```bash
git clone https://github.com/cxl-emu/OCEAN.git
cd OCEAN
bash ./script/setup_host.sh
# Assuming 2 hosts simulation. Change this based on the number of hosts you want to simulate:
bash ./script/setup_network.sh 2
cd qemu_integration
mkdir build
cd build
cmake ..
make -j$(nproc)
sudo make install
wget https://asplos.dev/about/qemu.img
wget https://asplos.dev/about/bzImage
cp qemu.img qemu1.img
./start_server.sh 9999 topology_simple.txt
sudo ../launch_qemu_cxl1.sh # login as root with password: victor129
# in qemu
vi /usr/local/bin/*.sh
# change 192.168.100.10 to 11
vi /etc/hostname
# change node0 to node1
shutdown now
# out of qemu
sudo ../launch_qemu_cxl.sh 
sudo ../launch_qemu_cxl1.sh 
```

## GROMACS
Change the hostfile in host 1 to reflect the number of hosts. 
```bash
cd workloads/gromacs
./build.sh
scp libmpi_cxl_shim.so  root@192.168.100.10:/root
scp libmpi_cxl_shim.so  root@192.168.100.11:/root
# Inside host 1:
export CXL_DAX_PATH="/dev/dax0.0"
export CXL_DAX_RESET=1  # Reset allocation counter on first process
export CXL_SHIM_VERBOSE=1
LD_PRELOAD=/root/libmpi_cxl_shim.so mpirun --allow-run-as-root -np 2 -hostfile hostfile -x CXL_DAX_PATH -x CXL_DAX_RESET -x CXL_SHIM_VERBOSE -x LD_PRELOAD ./gmx_mpi mdrun -s benchMEM.tpr -nsteps 10000 -resethway -ntomp 1
```


## TIGON
 
```bash
cd workloads/tigon
./scripts/setup.sh HOST
./emulation/image/make_vm_img.sh
sudo ./emulation/start_vms.sh --using-old-img --cxl 0 5 2 0 1 # using 2 hosts
./scripts/setup.sh VMS 2
./scripts/run.sh COMPILE_SYNC 2
./scripts/run_tpcc_dax.sh TwoPLPasha 2 3 mixed 10 15 1 0 1 Clock OnDemand 200000000 1 WriteThrough None 15 5 GROUP_WAL 20000 0 0
```


## OSU Benchmark
Change the hostfile in host 1 to reflect the number of hosts. 
```bash
# Inside host 1:
export CXL_DAX_PATH="/dev/dax0.0"
export CXL_DAX_RESET=1  # Reset allocation counter on first process
export CXL_SHIM_VERBOSE=1
LD_PRELOAD=/root/libmpi_cxl_shim.so mpirun --allow-run-as-root -np 2 -hostfile hostfile_cp -x CXL_DAX_PATH -x CXL_DAX_RESET -x CXL_SHIM_VERBOSE -x LD_PRELOAD ~/osu-micro-benchmarks/mpi/collective/osu_allgather
```
