#!/bin/bash

# check is script is sourced! source ./test.sh or . ./test.sh

script_name=$( basename -- "${0#-}" ) #- needed if sourced no path
this_script=$( basename -- "${BASH_SOURCE}" )
if [[ ${script_name} = "${this_script}" ]] ; then
    echo "This script need be run souced, [ . ./script.sh]"
    exit;
else
    echo "sourced from ${script_name}"
fi 

# base dir name
dir=code_env;

if [ ! -e $dir ]; then
    mkdir $dir
elif [ ! -d $dir ]; then
    echo "$dir already exists but is not a directory" 1>&2
    return;
else
    echo "$dir already exists. aboring" 1>&2
    return;
fi

cd $dir;
# clone pico-sdk and submodules 
git clone https://github.com/raspberrypi/pico-sdk.git --branch master
cd pico-sdk
git submodule update --init

#set path to pico-sdk
pico_sdk_path=$(pwd)
export PICO_SDK_PATH=$pico_sdk_path;

cd ..;

# clone and build picotools 
git clone https://github.com/raspberrypi/picotool.git
cd picotool;
export PICOTOOL_FETCH_FROM_GIT_PATH=$(pwd);
mkdir build;
cd build;
cmake ..
make
ln -s $(pwd)/picotool /usr/local/bin/picotool;
cd ../../

git clone https://github.com/raspberrypi/pico-examples.git --branch master
#cd pico-examples
#mkdir build
#cd build
#cmake ..
#cd ../..

export PICO_BOARD=pico2_w

git clone git@github.com:henrik-r217/rp_pico_2_w.git --branch main
cd rp_pico_2_w
git submodule update --init
cd ../..
