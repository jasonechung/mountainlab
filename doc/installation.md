### Supported operating systems:
Linux/Ubuntu or Debian is by far the easiest. Mac and other Linux flavors should also work. Windows is not necessarily excluded.

### There are four steps to installing MountainLab

* Install the prerequisites
* Clone and compile MountainLab
* Test the installation
* If necessary, contact Jeremy

### Step 1: Install the prerequisites

The mandatory prerequisites are:

* Qt5 (version 5.5 or later) – see below
* NodeJS
* FFTW
* Octave
* git
* Optionally you can install Matlab

See below for details on installing these packages.

### Step 2: Clone and compile MountainLab

First time:

> git clone https://github.com/magland/mountainlab.git

> cd mountainlab

> ./compile_components.sh

Subsequent updates:

> cd mountainlab

> git pull

> ./compile_components.sh

Add mountainlab/bin to your PATH environment variable. For example append the following to your ~/.bashrc file, and open a new terminal (or, source .bashrc):

> export PATH=/path/to/mountainlab/bin:$PATH

### Step 3: Test the installation

Prepare the example spike sorting:

> cd examples/003_kron_mountainsort

> ./001_generate_synthetic_data.sh

This will use matlab if you have it installed, otherwise it will use octave. It will generate 5 example synthetic datasets in an examples subdirectory, and the raw data are written to the BIGFILES subdirectory. Thus we begin following the principle of separating large files from their contents, as will be described in more detail.
Run the standard mountainsort processing pipeline

> kron-run ms2 example1

Finally, launch the viewer:

> kron-view results ms2 example1

See the other docs for details on what's going on here.

### Step 4: If necessary, contact Jeremy

I'm happy to help, and we can improve the docs

### Prerequisite: Install Qt5

If you are on a later version of Ubuntu (such as 16.04), you can get away with installing Qt5 using the package manager (which is great news). Otherwise, that method may not give a recent enough Qt5 version. In that case (for example if you are on 14.04, or using Mac or other Linux flavor), you should install Qt5 via qt.io website (see below).

If you've got Ubuntu 16.04 or later (good news):

> apt-get install software-properties-common

> apt-add-repository ppa:ubuntu-sdk-team/ppa

> apt-get update

> apt-get install qtdeclarative5-dev

> apt-get install qt5-default qtbase5-dev qtscript5-dev make g++

### Prerequisite: Install FFTW, Octave, and NodeJS

> apt-get install libfftw3-dev

> apt-get install octave

> apt-get install nodejs npm

> Or a one-liner: apt-get install -y libfft23-dev nodejs npm octave

### Prerequisite: If necessary, install Qt5 from qt.io

As mentioned above, if you are not using a later version of Ubuntu, you probably won't get a recent enough version from the package manager. In that case follow these directions to install a recent version of Qt5:

Go to https://www.qt.io/download/ and click through that you want to install the open source version. Download and run the appropriate installer. Note that you do not need to set up a Qt account -- you can skip that step in the install wizard.

We recommend installing this in your home directory, which does not require admin privileges.

Once installed you will need to prepend the path to qmake to your PATH environment variable. On my system that is /home/magland/Qt/5.7/gcc_64/bin.
