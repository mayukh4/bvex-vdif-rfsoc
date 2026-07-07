#!/bin/bash
export PATH=$HOME/difx/env/bin:$HOME/difx/bin:$PATH
export LD_LIBRARY_PATH=$HOME/difx/lib:$HOME/difx/env/lib
cd ~/bvex_vdif
rm -rf bvex_1.difx
mpirun -np 4 --oversubscribe mpifxcorr bvex_1.input > mpifx.log 2>&1
echo "exit=$?" > mpifx.done
