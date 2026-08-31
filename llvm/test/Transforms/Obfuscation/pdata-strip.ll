; Verify that pdata-strip removes the uwtable attribute and a dead personality
; from a function with no exception handling, while leaving a function that
; actually uses EH untouched.

; REQUIRES: plugins
; RUN: opt -load-pass-plugin=%llvmshlibdir/LLVMObfuscationPlugin%pluginext \
; RUN:     -passes=pdata-strip -S %s | FileCheck %s

declare i32 @__gxx_personality_v0(...)
declare void @may_throw()

; The unwind table and personality are both dead here and must be dropped, so
; the definition carries no attribute group or personality clause afterwards.
; CHECK: define void @plain() {
define void @plain() uwtable personality ptr @__gxx_personality_v0 {
  ret void
}

; This function's personality is load-bearing (it has an invoke/landingpad) and
; must be preserved.
; CHECK: define void @with_eh() personality ptr @__gxx_personality_v0
define void @with_eh() personality ptr @__gxx_personality_v0 {
entry:
  invoke void @may_throw() to label %ok unwind label %lpad
ok:
  ret void
lpad:
  %e = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %e
}

; The only uwtable in the module belonged to @plain and is now gone.
; CHECK-NOT: uwtable
