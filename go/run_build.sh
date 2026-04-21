#!/bin/bash
cd /home/stheera/atlas-scenarios/deps/ns-3/contrib/ndndSIM/go
export PATH="/home/stheera/go/pkg/mod/golang.org/toolchain@v0.0.1-go1.24.3.linux-amd64/bin:$PATH"
export GO="/home/stheera/go/pkg/mod/golang.org/toolchain@v0.0.1-go1.24.3.linux-amd64/bin/go"
bash build.sh > /tmp/build_final_v2.log 2>&1
