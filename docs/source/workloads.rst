===========================
Workloads & Experiments
===========================

This page describes the workloads available in OCEAN and how to run experiments with them.

.. note::
   
   This guide assumes you have completed :doc:`getting-started` and :doc:`configuration`.
   Your CXL server should be running and VMs should be connected.

Overview
========

OCEAN includes workloads across multiple computational domains:

* **GROMACS** - Molecular dynamics for scientific computing
* **TIGON** - Distributed database with TPC-C benchmark
* **OSU Benchmarks** - MPI collective communication tests

How Workloads Use CXL Memory
-----------------------------

All workloads access CXL memory transparently through the MPI shim library:

.. code-block:: text

   Application
       ↓
   MPI Shim (LD_PRELOAD)
       ↓
   /dev/dax0.0
       ↓
   QEMU VM
       ↓
   CXL Server
       ↓
   Memory Pools

The shim intercepts memory allocation calls (``malloc``, ``calloc``, etc.) and redirects
them to CXL memory without requiring application code changes.

Common Setup
============

Environment Variables
---------------------

Set these variables before running any workload:

.. code-block:: bash

   export CXL_DAX_PATH="/dev/dax0.0"
   export CXL_DAX_RESET=1
   export CXL_SHIM_TRACE=1      # Optional: enable tracing
   export CXL_SHIM_VERBOSE=1    # Optional: verbose output

.. list-table:: Variable Reference
   :header-rows: 1
   :widths: 30 70

   * - Variable
     - Purpose
   * - ``LD_PRELOAD``
     - Path to shim library (set per workload)
   * - ``CXL_DAX_PATH``
     - CXL device path (default: ``/dev/dax0.0``)
   * - ``CXL_DAX_RESET``
     - Reset allocation counter (set to ``1`` for first process)
   * - ``CXL_SHIM_TRACE``
     - Enable detailed tracing (``1`` = enabled, ``0`` = disabled)
   * - ``CXL_SHIM_VERBOSE``
     - Enable verbose output (``1`` = enabled, ``0`` = disabled)

Hostfile Configuration
----------------------

For multi-host experiments, create a hostfile with node IPs:

.. code-block:: text

   # hostfile
   192.168.100.10
   192.168.100.11

With slot specification (optional):

.. code-block:: text

   192.168.100.10 slots=4
   192.168.100.11 slots=4

GROMACS
=======

**What:** Molecular dynamics simulation for biomolecular research.

**Why:** Tests CXL memory with scientific computing workloads and multi-host MPI execution.

Building
--------

.. code-block:: bash

   cd workloads/gromacs
   ./build.sh

Distribute the shim library to all nodes:

.. code-block:: bash

   scp libmpi_cxl_shim.so root@192.168.100.10:/root
   scp libmpi_cxl_shim.so root@192.168.100.11:/root

Running
-------

.. code-block:: bash

   cd workloads/gromacs
   
   mpirun --allow-run-as-root \
          -x CXL_SHIM_TRACE \
          -x CXL_DAX_PATH \
          -x LD_PRELOAD=$PWD/libmpi_cxl_shim.so \
          --hostfile ./hostfile \
          ./gromacs-2025.3/build/bin/gmx_mpi mdrun \
          -s benchMEM.tpr \
          -nsteps 10000 \
          -ntomp 1

Key Parameters
--------------

* ``-s benchMEM.tpr`` - Simulation input file
* ``-nsteps 10000`` - Number of simulation steps
* ``-ntomp 1`` - OpenMP threads per MPI rank

Expected Output
---------------

.. code-block:: text

   Starting mdrun on 2 ranks
   ...
   Performance: 123.4 ns/day, 0.194 hours/ns
   ...

TIGON
=====

**What:** Distributed database with ``io_uring`` integration and TPC-C benchmark.

**Why:** Tests database workloads with CXL memory and transaction processing.

Setup
-----

.. code-block:: bash

   cd workloads/tigon
   ./scripts/setup.sh HOST
   ./emulation/image/make_vm_img.sh

Start VMs with CXL support:

.. code-block:: bash

   sudo ./emulation/start_vms.sh --using-old-img --cxl 0 5 2 0 1

.. note::
   
   ``--using-old-img`` reuses existing VM images. ``--cxl`` parameters configure
   CXL device settings (device ID, memory size, host count, etc.).

Configure VMs:

.. code-block:: bash

   ./scripts/setup.sh VMS 2
   ./scripts/run.sh COMPILE_SYNC 2

Running TPC-C
-------------

.. code-block:: bash

   ./scripts/run_tpcc_dax.sh TwoPLPasha 2 3 mixed 10 15 1 0 1 \
                             Clock OnDemand 200000000 1 WriteThrough \
                             None 15 5 GROUP_WAL 20000 0 0

Key Parameters
--------------

* ``TwoPLPasha`` - Concurrency control protocol
* ``2`` - Number of hosts
* ``3`` - Warehouses per host
* ``mixed`` - Workload type (read/write mix)

Expected Output
---------------

.. code-block:: text

   Throughput: 1234 txn/s
   Latency (mean): 12.3 ms
   Latency (p99): 45.6 ms
   Abort rate: 2.1%

OSU Benchmarks
==============

**What:** MPI communication performance benchmarks (latency, bandwidth, collectives).

**Why:** Tests MPI operations with CXL-backed memory and validates multi-host communication.

Installation
------------

.. code-block:: bash

   # Inside VM
   wget https://mvapich.cse.ohio-state.edu/download/mvapich/osu-micro-benchmarks-7.4.tar.gz
   tar -xzf osu-micro-benchmarks-7.4.tar.gz
   cd osu-micro-benchmarks-7.4
   ./configure CC=mpicc
   make

Running a Benchmark
-------------------

.. code-block:: bash

   export CXL_DAX_PATH="/dev/dax0.0"
   export CXL_DAX_RESET=1
   
   LD_PRELOAD=/root/libmpi_cxl_shim.so \
   mpirun --allow-run-as-root \
          -np 2 \
          -hostfile hostfile \
          -x CXL_DAX_PATH \
          -x CXL_DAX_RESET \
          -x LD_PRELOAD \
          ~/osu-micro-benchmarks/mpi/collective/osu_allgather

Available Benchmarks
--------------------

**Collective Operations:**

* ``osu_allgather``, ``osu_allreduce``, ``osu_alltoall``
* ``osu_barrier``, ``osu_bcast``, ``osu_reduce``

**Point-to-Point:**

* ``osu_latency``, ``osu_bw``, ``osu_bibw``

Expected Output
---------------

.. code-block:: text

   # OSU MPI Allgather Test
   # Size       Avg Latency(us)
   1                      12.34
   2                      13.45
   4                      14.56
   ...

Monitoring & Debugging
======================

During Execution
----------------

**Check CXL device:**

.. code-block:: bash

   ls -l /dev/dax0.0

**Monitor resources:**

.. code-block:: bash

   free -h
   numactl --hardware

**Server logs:**

The CXL server outputs connection and memory access statistics to stdout.

Enable Detailed Logging
------------------------

.. code-block:: bash

   export SPDLOG_LEVEL=debug
   export CXL_SHIM_TRACE=1
   export CXL_SHIM_VERBOSE=1

Common Issues
-------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Issue
     - Solution
   * - Workload fails to start
     - Verify server running, ``/dev/dax0.0`` exists, shim path correct
   * - MPI communication errors
     - Check hostfile IPs, verify VMs can ping each other
   * - Memory allocation failures
     - Set ``CXL_DAX_RESET=1``, check server logs, verify memory pool size
   * - Poor performance
     - Check VM resources, network connectivity, topology configuration

Debugging Commands
------------------

**Verify environment:**

.. code-block:: bash

   mpirun ... env | grep CXL

**Check MPI rank mapping:**

.. code-block:: bash

   mpirun --display-map ...

**Test connectivity:**

.. code-block:: bash

   ping 192.168.100.11

Next Steps
==========

After running experiments:

1. **Review Output** - Check application performance metrics
2. **Analyze Traces** - If ``CXL_SHIM_TRACE=1`` was set, examine memory access patterns
3. **Compare Results** - Run with/without CXL to measure benefits
4. **Tune Configuration** - Adjust topology, memory allocation, or host count
5. **Scale Up** - Add more hosts to test larger configurations

For detailed analysis:

* Server logs contain CXL fabric statistics
* Application output shows workload-specific metrics
* System tools (``perf``, ``numactl``) provide additional insights