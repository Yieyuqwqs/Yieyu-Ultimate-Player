# build.sh 的糖。所有逻辑仍在 tools/orchestrate.mjs。
.PHONY: all doctor apk native rust go ts wasm lua idl gradle verify clean distclean

all:      ; ./build.sh
doctor:   ; ./build.sh doctor
idl:      ; ./build.sh idl
rust:     ; ./build.sh rust
go:       ; ./build.sh go
ts:       ; ./build.sh ts
wasm:     ; ./build.sh wasm
native:   ; ./build.sh native
gradle:   ; ./build.sh gradle
apk:      ; ./build.sh flutter verify
verify:   ; ./build.sh verify
dry:      ; ./build.sh --dry-run
clean:    ; rm -rf out/apk out/native out/gen
distclean:; rm -rf out
