;; Minimal ACI instrument written by hand in WebAssembly text format.
;; Monophonic square wave; demonstrates that ACI is language-agnostic.
;; Build: wat2wasm minimal.wat -o minimal.wasm  (or: aicontact.py deploy minimal.wat)
(module
  (memory (export "memory") 1)
  ;; metadata JSON at offset 16, output buffer (256 floats) at offset 1024
  (data (i32.const 16) "{\"aci\":1,\"id\":\"aicontact.example.wat-square\",\"name\":\"WAT Square\",\"version\":\"1.0.0\",\"outputs\":1}\00")
  (global $sr (mut f32) (f32.const 48000))
  (global $phase (mut f32) (f32.const 0))
  (global $inc (mut f32) (f32.const 0))
  (global $amp (mut f32) (f32.const 0))
  (global $key (mut i32) (i32.const -1))

  (func (export "aci_abi_version") (result i32) (i32.const 1))
  (func (export "aci_meta") (result i32) (i32.const 16))
  (func (export "aci_init") (param $sr f32) (param $max i32) (result i32)
    (global.set $sr (local.get $sr)) (i32.const 0))
  (func (export "aci_output") (param $ch i32) (result i32) (i32.const 1024))

  ;; freq = 440 * 2^((key-69)/12), computed by repeated multiplication (no pow needed)
  (func $freq (param $key i32) (result f32)
    (local $f f32) (local $n i32)
    (local.set $f (f32.const 8.175799))            ;; key 0
    (local.set $n (local.get $key))
    (block $done (loop $l
      (br_if $done (i32.le_s (local.get $n) (i32.const 0)))
      (local.set $f (f32.mul (local.get $f) (f32.const 1.0594631)))
      (local.set $n (i32.sub (local.get $n) (i32.const 1)))
      (br $l)))
    (local.get $f))

  (func (export "aci_note_on") (param $key i32) (param $vel f32)
    (global.set $key (local.get $key))
    (global.set $inc (f32.div (call $freq (local.get $key)) (global.get $sr)))
    (global.set $amp (f32.mul (local.get $vel) (f32.const 0.2))))
  (func (export "aci_note_off") (param $key i32) (param $vel f32)
    (if (i32.eq (local.get $key) (global.get $key))
      (then (global.set $amp (f32.const 0)) (global.set $key (i32.const -1)))))

  (func (export "aci_process") (param $frames i32)
    (local $i i32)
    (block $done (loop $l
      (br_if $done (i32.ge_s (local.get $i) (local.get $frames)))
      (global.set $phase (f32.add (global.get $phase) (global.get $inc)))
      (if (f32.ge (global.get $phase) (f32.const 1))
        (then (global.set $phase (f32.sub (global.get $phase) (f32.const 1)))))
      (f32.store
        (i32.add (i32.const 1024) (i32.shl (local.get $i) (i32.const 2)))
        (select (global.get $amp) (f32.neg (global.get $amp))
                (f32.lt (global.get $phase) (f32.const 0.5))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $l))))
)
