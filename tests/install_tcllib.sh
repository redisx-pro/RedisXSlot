#!/bin/bash
set -e

wget https://core.tcl-lang.org/tcllib/uv/tcllib-1.21.tar.gz
tar zxvf tcllib-1.21.tar.gz
cd tcllib-1.21
echo "y" | sudo tclsh installer.tcl



