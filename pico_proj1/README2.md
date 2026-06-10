# README in buld folder. 

Add information about how to build the project.


export PICO_SDK_PATH=~/code/rp_pico_2w/pico_proj1/pico-sdk

#Fixa

export PICOTOOL_FETCH_FROM_GIT_PATH=~/code/rp_pico_2w/xxxxx/pico_proj1/pico-sdk


cmake -DPICO_BOARD=pico2_w ..




CMake Warning at pico-sdk/tools/Findpicotool.cmake:30 (message):
  No installed picotool with version 2.1.1 found - building from source

  It is recommended to build and install picotool separately, or to set
  PICOTOOL_FETCH_FROM_GIT_PATH to a common directory for all your SDK
  projects

Get picotools:

git clone https://github.com/raspberrypi/picotool.git

You need libusb:
sudo apt install libusb-1.0-0-dev

Building picotool:

$ mkdir build
$ cd build
$ export PICO_SDK_PATH=~/pico/pico-sdk
$ cmake ../
$ make
this will generate a picotool command-line binary in the build/picotool directory.


Pico-sdk and pico-examples:

$ cd ~/pico
$ git clone https://github.com/raspberrypi/pico-sdk.git --branch master
$ cd pico-sdk
$ git submodule update --init
$ cd ..
$ git clone https://github.com/raspberrypi/pico-examples.git --branch master


Install Toolchain

$ sudo apt update

$ sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential

Ubuntu and Debian users might additionally need to do:

$ apt install g++ libstdc++-arm-none-eabi-newlib