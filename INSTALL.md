do 24 feb 2022  1:25:28 CET
# SAGECal Installation

## Cmake Build
#### Ubuntu 20.04 (quick install)
```
 sudo apt-get install -y git cmake g++ pkg-config libcfitsio-bin libcfitsio-dev libopenblas-base libopenblas-dev wcslib-dev wcslib-tools libglib2.0-dev libcasa-casa4 casacore-dev casacore-data casacore-tools gfortran libopenmpi-dev libfftw3-dev

```
Run cmake (with GPU support) for example like
```
 mkdir build && cd build
 cmake .. -DHAVE_CUDA=ON -DCMAKE_CXX_FLAGS='-DMAX_GPU_ID=0' -DCMAKE_CXX_COMPILER=g++-8  -DCMAKE_C_FLAGS='-DMAX_GPU_ID=0' -DCMAKE_C_COMPILER=gcc-8 -DCUDA_NVCC_FLAGS='-gencode arch=compute_75,code=sm_75' -DBLA_VENDOR=OpenBLAS
```
where *MAX_GPU_ID=0* is when there is only one GPU (ordinal 0). If you have more GPUs, increase this number to 1,2, and so on. This will produce *sagecal_gpu* and *sagecal-mpi_gpu* binary files (after running *make* of course).

CPU only version can be build as
```
 cmake .. -DCMAKE_CXX_COMPILER=g++-8 -DCMAKE_C_COMPILER=gcc-8 -DBLA_VENDOR=OpenBLAS
```
which will produce *sagecal* and *sagecal-mpi*.

The option *-DBLA_VENDOR=OpenBLAS* is to select OpenBLAS explicitly, but other BLAS  flavours can also be given. If not specified, whatever BLAS installed will be used.

If you get **-lgfortran is not found** error, run the following in the build directory
```
 cd dist/lib
 ln -s /usr/lib/x86_64-linux-gnu/libgfortran.so.5 libgfortran.so
```
to make a symbolic link to libgfortran.so.5 or whatever version that is installed.

To only build *libdirac* (shared) library, use *-DLIB_ONLY=1* option (also *-DBLA_VENDOR* to select the BLAS flavour). This library can be used with pkg-config using *lib/pkgconfig/libdirac.pc*.

### Requirements for older installations
#### das5

Load the modules below before compiling SageCal.
```
module load cmake/3.8.2
module load mpich/ge/gcc/64/3.2
module load gcc/4.9.3
module load casacore/2.3.0-gcc-4.9.3
module load wcslib/5.13-gcc-4.9.3
module load wcslib/5.16-gcc-4.9.3
module load cfitsio/3.410-gcc-4.9.3
```

checkout the source code and compile it with the instructions below(in source folder):
```
git clone https://github.com/nlesc-dirac/sagecal.git

cd sagecal && mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$INSTALL_PATH
make
make install
```
$INSTALL_PATH is where you want to install SageCal.

#### Other systems

- Install equivalent packages for your distribution
    - g++
    - cmake
    - git
    - pkg-config
    - openblas
    - libglib2.0-dev
    - follow the instructions at
[https://github.com/casacore/casacore](https://github.com/casacore/casacore) to install casacore.
    - Additional packages (not essential, but recommended): MPI (openmpi), FFTW



### Building
- Clone the repository
```
    git clone -b master https://git@github.com/nlesc-dirac/sagecal.git

```

- Build SAGECal
```
    mkdir build && cd build
    cmake ..
```

**OPTIONAL:** You can also define a custom casacore path:

```
    cmake .. -DCASACORE_ROOT_DIR=/opt/soft/casacore
```
**OPTIONAL:** You can also define a custom paths to everything:

```
    cmake -DCFITSIO_ROOT_DIR=/cm/shared/package/cfitsio/3380-gcc-4.9.3 -DCASACORE_ROOT_DIR=/cm/shared/package/casacore/v2.3.0-gcc-4.9.3 -DWCSLIB_INCLUDE_DIR=/cm/shared/package/wcslib/5.13-gcc-4.9.3/include -DWCSLIB_LIBRARY=/cm/shared/package/wcslib/5.13-gcc-4.9.3/lib/libwcs.so -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON -DCMAKE_LINKER=/cm/shared/package/gcc/4.9.3/bin/gcc -DCMAKE_CXX_FLAGS=-L/cm/shared/package/cfitsio/3380-gcc-4.9.3/lib -DCMAKE_C_FLAGS=-L/cm/shared/package/cfitsio/3380-gcc-4.9.3/lib ..
```

    Compile with:
```
    make
```
    Install at your favorite place
```
    make DEST=/path/to/sagecal/dir install
```

- The sagecal executable can be found in **/path/to/sagecal/dir/usr/local/bin**, also **sagecal-mpi**,**buildsky** and **restore** might be installed depending on the availability of MPI and WCSLIB/FFTW.

### MPI support
MPI support is automatically detected, otherwise, it can be forced with:
```
cmake -DENABLE_MPI=ON
```

## GPU Support

### Loading modules on DAS5
See scripts folder for the modules.
```
source ./scripts/load_das5_modules_gcc6.sh
```

### Compiling with GPU support
```
mkdir -p build && cd build
cmake -DCUDA_DEBUG=ON -DDEBUG=ON -DHAVE_CUDA=ON ..
make VERBOSE=1
```



## Installation via Anaconda (WIP)
```
    conda install -c sagecal=0.6.0
```



## Manual installation
For expert users, and for custom architectures (GPU), the manual install is recommended.
### 1 Prerequisites:
 - CASACORE http://casacore.googlecode.com/
 - glib http://developer.gnome.org/glib
 - BLAS/LAPACK
   Highly recommended is OpenBLAS http://www.openblas.net/
   Also, to avoid any linking issues (and to get best performance), build OpenBLAS from source and link SAGECal with the static library (libopenblas***.a) and NOT libopenblas***.so
 - Compilers gcc/g++ or Intel icc/icpc
 - If you have NVIDIA GPUs,
  -- CUDA/CUBLAS/CUSOLVER and nvcc
  -- NVML Nvidia management library
 - If you are using Intel Xeon Phi MICs.
  -- Intel MKL and other libraries
 - Get the source for SAGECal
```
    git clone -b master https://git@github.com/nlesc-dirac/sagecal.git
```

### 2 The basic way to build is
  1.a) go to ./src/lib/Dirac and ./src/lib/Radio  and run make (which will create libdirac.a and libradio.a)
  1.b) go to ./src/MS and run make (which will create the executable)


### 3 Build settings
In ./src/lib/Dirac and ./src/lib/Radio and ./src/MS you MUST edit the Makefiles to suit your system. Some common items to edit are:
 - LAPACK: directory where LAPACK/OpenBLAS is installed
 - GLIBI/GLIBL: include/lib files for glib
 - CASA_LIBDIR/CASA_INCDIR/CASA_LIBS : casacore include/library location and files:
  Note with new CASACORE might need two include paths, e.g.
    -I/opt/casacore/include/ -I/opt/casacore/include/casacore
 - CUDAINC/CUDALIB : where CUDA/CUBLAS/CUSOLVER is installed
 - NVML_INC/NVML_LIB : NVML include/lib path
 - NVCFLAGS : flags to pass to nvcc, especially -arch option to match your GPU
 - MKLROOT : for Intel MKL

 Example makefiles:
   Makefile : plain build
   Makefile.gpu: with GPU support
   Note: Edit ./lib/Radio/Radio.h MAX_GPU_ID to match the number of available GPUs, e.g., for 2 GPUs, MAX_GPU_ID=1



## SAGECAL-MPI Manual Installation
This is for manually installing the distributed version of sagecal (sagecal-mpi), the cmake build will will work for most cases.
## 1 Prerequsites:
 - Same as for SAGECal.
 - MPI (e.g. OpenMPI)

## 2 Build ./src/lib/Dirac ./src/lib/Radio as above (using mpicc -DMPI_BUILD)

## 3 Build ./src/MPI using mpicc++



## BUILDSKY Installation

  - See INSTALL in ./src/buildsky


## RESTORE Installation

  - See INSTALL in ./src/restore



