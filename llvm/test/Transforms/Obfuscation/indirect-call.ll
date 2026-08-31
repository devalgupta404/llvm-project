; Verify that indirect-call reroutes a direct call through the module-level
; function-pointer table using a volatile load, so no direct edge remains.

; REQUIRES: plugins
; RUN: opt -load-pass-plugin=%llvmshlibdir/LLVMObfuscationPlugin%pluginext \
; RUN:     -passes=indirect-call -S %s | FileCheck %s

; CHECK: @__indirect_call_table = internal global [1 x ptr] [ptr @callee]

define internal i32 @callee(i32 %x) {
  ret i32 %x
}

; CHECK-LABEL: define i32 @caller(
; CHECK: %ind.slot = getelementptr inbounds [1 x ptr], ptr @__indirect_call_table, i32 0, i32 0
; CHECK: %ind.fp = load volatile ptr, ptr %ind.slot
; CHECK: call i32 %ind.fp(i32 %a)
; CHECK-NOT: call i32 @callee
define i32 @caller(i32 %a) {
  %r = call i32 @callee(i32 %a)
  ret i32 %r
}
