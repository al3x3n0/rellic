#!/bin/bash
set -ex

# First use LLVM image to compile
docker run --rm -v $(pwd):/work -w /work silkeh/clang:16 bash -c '
    # Compile to LLVM IR
    clang -S -emit-llvm -O0 -g test.c -o test.ll
    # Convert to bitcode
    llvm-as test.ll -o test.bc
    # Run reg2mem pass
    # opt -passes=reg2mem test.bc -o test_reg2mem.bc
'

docker run --rm -v /Users/al3x3n0/huawei/rellic:/work -w /work --name rellic-decomp --entrypoint bash rellic -c 'echo "=== Running rellic-decomp ===" && /opt/trailofbits/bin/rellic-decomp --input test.bc --output test_decompiled.c --output-mapping --mapping-output test_mapping.json --logtostderr --v=3 --minloglevel=0'

# Can we print docker logs after the container exits?
#docker logs rellic-decomp

# Show results
echo "=== LLVM IR ==="
cat test.ll
echo
echo "=== Decompiled C code ==="
cat test_decompiled.c
echo
echo "=== Basic block to line mapping ==="
cat test_mapping.json 
