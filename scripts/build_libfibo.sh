#!/bin/sh

# make sure we are in the right directory
RUN_DIR=$(basename $(pwd))

if [ $RUN_DIR == "scripts" ];
then
	cd ..
	RUN_DIR=$(basename $(pwd))
fi

if [ $RUN_DIR != "libpulp" ];
then
	echo "Please, run this from libpulp root directory"
	exit
fi

# uninstal rpms, for when retrying
sudo rpm -e fiboapp-0.1-0.x86_64
sudo rpm -e libfibo-0.1-0.x86_64

# build and install libpulp and the fibo example
rpmbuild -bb libfibo.spec
sudo rpm -ivh rpmbuild/rpms/x86_64/libfibo-0.1-0.x86_64.rpm
sudo rpm -ivh rpmbuild/rpms/x86_64/fiboapp-0.1-0.x86_64.rpm
rpmbuild -bb libfibo_livepatch.spec
