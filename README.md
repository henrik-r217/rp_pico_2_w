# rp_pico_2_w



mkdir ~/code/pico

cd ~/code/pico

git clone https://github.com/raspberrypi/pico-sdk.git --branch master

cd pico-sdk

git submodule update --init

cd ..

git clone https://github.com/raspberrypi/pico-examples.git --branch master

cd pico-examples
$ mkdir build
$ cd build

export PICO_SDK_PATH=~/code/pico/pico-sdk

cmake -DPICO_BOARD=pico2_w  ..


Pico project: /home/henrik/code/pico_proj1

In project folder 

git clone https://github.com/raspberrypi/pico-sdk.git

Copy external/pico_sdk_import.cmake from the SDK into your project directory

export PICO_SDK_PATH='~'/code/pico_proj1/pico-sdk

alt: -DPICO_SDK_PATH='~'/code/pico_proj1/pico-sdk

pwd:/home/henrik/code/pico_proj1

cp  pico-sdk/external/pico_sdk_import.cmake .

mkdir build
cd build
cmake -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=~/code/pico_proj1/pico-sdk ..


-DWIFI_SSID="Telia-9F1ED6"

-DWIFI_PASSWORD="aTuptyeMvx89neCd"

cmake -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=~/code/pico_proj1/pico-sdk -DWIFI_SSID="Telia-9F1ED6" -DWIFI_PASSWORD="aTuptyeMvx89neCd" ..


minicom -b 115200 -o -D /dev/ttyACM0