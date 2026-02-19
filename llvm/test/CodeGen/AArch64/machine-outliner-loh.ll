; RUN: llc -verify-machineinstrs -mtriple=aarch64-apple-darwin < %s | FileCheck %s --implicit-check-not=.loh --check-prefixes=CHECK,LOH
; RUN: llc -verify-machineinstrs -mtriple=aarch64-apple-darwin -enable-machine-outliner < %s | FileCheck %s --implicit-check-not=.loh --check-prefixes=CHECK,OUTLINE

@A = global i32 0, align 4
@B = global i32 0, align 4

declare void @foo();
declare void @bar(ptr %a);
declare void @goo(ptr %a);

; CHECK-LABEL: _a0:
define void @a0(i32 %a) {

  ; This becomes AdrpAddLdr when outlining is disabled, otherwise it is outlined
  ; and there should be no LOH.
  %addr = getelementptr inbounds i32, ptr @A, i32 0
  %res = load i32, ptr %addr, align 4
  ; LOH:      [[L0:Lloh.+]]:
  ; LOH-NEXT:   adrp x8, _A@PAGE
  ; LOH-NEXT: [[L1:Lloh.+]]:
  ; LOH-NEXT:   add x8, x8, _A@PAGEOFF
  ; LOH-NEXT: [[L2:Lloh.+]]:
  ; LOH-NEXT:   ldr w19, [x8]

  call void @foo()
  ; LOH:           bl _foo
  ; LOH-NEXT: [[L3:Lloh.+]]:
  ; LOH-NEXT:   adrp x0, _A@PAGE
  ; LOH-NEXT: [[L4:Lloh.+]]:
  ; LOH-NEXT:   add x0, x0, _A@PAGEOFF
  ; OUTLINE:      bl _OUTLINED_FUNCTION_0
  ; OUTLINE-NEXT: [[OL0:Lloh.+]]:
  ; OUTLINE-NEXT:   adrp x0, _A@PAGE
  ; OUTLINE-NEXT: [[OL1:Lloh.+]]:
  ; OUTLINE-NEXT:   add x0, x0, _A@PAGEOFF
  ; OUTLINE-NEXT: bl _bar
  call void @bar(ptr %addr)

  ; This becomes AdrpAddStr.
  %addr2 = getelementptr inbounds i32, ptr @B, i32 4
  store i32 %res, ptr %addr2, align 4
  ; CHECK:      [[L5:Lloh.+]]:
  ; CHECK-NEXT:   adrp x8, _B@PAGE
  ; CHECK-NEXT: [[L6:Lloh.+]]:
  ; CHECK-NEXT:   add x8, x8, _B@PAGEOFF
  ; CHECK-NEXT: [[L7:Lloh.+]]:
  ; CHECK-NEXT:   str w19, [x8, #16]
  ret void

  ; LOH-DAG:   .loh AdrpAddLdr [[L0]], [[L1]], [[L2]]
  ; LOH-DAG:   .loh AdrpAdd [[L3]], [[L4]]
  ; OUTLINE-DAG: .loh AdrpAdd [[OL0]], [[OL1]]
  ; CHECK-DAG: .loh AdrpAddStr [[L5]], [[L6]], [[L7]]
  ; CHECK:     .cfi_endproc
}

; CHECK-LABEL: _a1:
define i32 @a1(i32 %a) {

  ; This becomes AdrpAddLdr when outlining is disabled, otherwise it is outlined
  ; and there should be no LOH.
  %addr = getelementptr inbounds i32, ptr @A, i32 0
  %res = load i32, ptr %addr, align 4
  ; LOH:      [[L8:Lloh.+]]:
  ; LOH-NEXT:   adrp x8, _A@PAGE
  ; LOH-NEXT: [[L9:Lloh.+]]:
  ; LOH-NEXT:   add x8, x8, _A@PAGEOFF
  ; LOH-NEXT: [[L10:Lloh.+]]:
  ; LOH-NEXT:   ldr w19, [x8]

  call void @foo()
  ; LOH:           bl _foo
  ; LOH-NEXT: [[L11:Lloh.+]]:
  ; LOH-NEXT:   adrp x0, _A@PAGE
  ; LOH-NEXT: [[L12:Lloh.+]]:
  ; LOH-NEXT:   add x0, x0, _A@PAGEOFF
  ; OUTLINE:      bl _OUTLINED_FUNCTION_0
  ; OUTLINE-NEXT: [[OL2:Lloh.+]]:
  ; OUTLINE-NEXT:   adrp x0, _A@PAGE
  ; OUTLINE-NEXT: [[OL3:Lloh.+]]:
  ; OUTLINE-NEXT:   add x0, x0, _A@PAGEOFF
  ; OUTLINE-NEXT: bl _goo
  call void @goo(ptr %addr)
  ret i32 %res

  ; LOH-DAG:   .loh AdrpAddLdr [[L8]], [[L9]], [[L10]]
  ; LOH-DAG:   .loh AdrpAdd [[L11]], [[L12]]
  ; OUTLINE-DAG: .loh AdrpAdd [[OL2]], [[OL3]]
  ; CHECK: .cfi_endproc
}

; Note: it is not safe to add LOHs to this function as outlined functions do not
; follow calling convention and thus x19 could be live across the call.
; OUTLINE: _OUTLINED_FUNCTION_0:
; OUTLINE:   adrp x8, _A@PAGE
; OUTLINE:   add x8, x8, _A@PAGEOFF
; OUTLINE:   ldr w19, [x8]
; OUTLINE:   b _foo
