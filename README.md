# rp_pico_2_w



## folder sturecture 

```text
~/repo/
├──pico-examples
├──pico-sdk
├──picotool
└──README.md
```

### set enviroment 
```
export PICO_SDK_PATH=~/repo/pico-sdk"
export PICOTOOL_FETCH_FROM_GIT_PATH=~/repo/picotool
export PICO_BOARD=pico2_w

```
Or pass as argument to cmake  
```
cmake -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=~/code/pico_proj1/pico-sdk ..
```


### Get pico-sdk 

The pico-sdk contains submodules, nested repositories. 

```
git clone https://github.com/raspberrypi/pico-sdk.git --branch master
cd pico-sdk
git submodule update --init
```
### Get picotools
```
git clone https://github.com/raspberrypi/picotool.git
```
You need libusb: `sudo apt install libusb-1.0-0-dev`
```
mkdir build
cd build
```
Make sure that environment variables `PICO_SDK_PATH` is set 
```
cmake ..
make
```
This will generate a picotool command-line binary in the build/picotool directory.

```
sudo ~/repo/picotool/build/picotool info -a
```

### Get pico-examples 

```
git clone https://github.com/raspberrypi/pico-examples.git --branch master
cd pico-examples
mkdir build
cd build
```
Make sure that environment variables `PICO_BOARD` is set 
```
cmake ..
```


### setup project 

In project folder 

- Copy `external/pico_sdk_import.cmake` from the SDK into your project directory

### wifi setup variables 

```
-DWIFI_SSID="Telia-9F1ED6"
-DWIFI_PASSWORD="aTuptyeMvx89neCd"

cmake -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=~/code/pico_proj1/pico-sdk -DWIFI_SSID="Telia-9F1ED6" -DWIFI_PASSWORD="aTuptyeMvx89neCd" ..
```     
### terminal setting, USB mode 
```
sudo minicom -b 115200 -o -D /dev/ttyACM0
```
To quit `ctrl+a`,`z` then `x`

### Install Toolchain 

```
$ sudo apt update

$ sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential
```
Ubuntu and Debian users might additionally need to do:
```
$ apt install g++ libstdc++-arm-none-eabi-newlib
```
