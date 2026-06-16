


## Programs to be installed 
```
sudo apt install git 
sudo apt install emacs
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential
sudo apt install g++ libstdc++-arm-none-eabi-newlib
```

## Generating a new SSH key
```
ssh-keygen -t ed25519 -C "henrik@lindenet.se"
```

## Start the ssh-agent in the background.
```
eval "$(ssh-agent -s)"
```
## Add your SSH private key to the ssh-agent.
```
ssh-add ~/.ssh/id_ed25519
```
## Copy the SSH public key to your clipboard.
```
cat ~/.ssh/id_ed25519.pub 
```
## Test the SSH connection  
```
ssh -T git@github.com
```
## Set git config 
```
git config --global user.name "Henrik"
git config --global user.email henrik@lindenet.se
```


## Create file: .gitignore
```
# Ignore everything in this directory
*
# Except this file
!.gitignore
```
