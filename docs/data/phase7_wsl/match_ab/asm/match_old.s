0000000000012930 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)>:
   12930:	endbr64
   12934:	push   %r15
   12936:	mov    %rdi,%r15
   12939:	push   %r14
   1293b:	push   %r13
   1293d:	mov    %r9,%r13
   12940:	push   %r12
   12942:	push   %rbp
   12943:	push   %rbx
   12944:	sub    $0x48,%rsp
   12948:	test   %cl,%cl
   1294a:	mov    0x80(%rsp),%r12
   12952:	mov    %rsi,0x20(%rsp)
   12957:	mov    %edx,0xc(%rsp)
   1295b:	mov    %r8,0x28(%rsp)
   12960:	mov    %r12,%r14
   12963:	mov    %cl,0x3e(%rsp)
   12967:	sete   0x3f(%rsp)
   1296c:	test   %r13,%r13
   1296f:	jle    12c70 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x340>
   12975:	cmpb   $0x0,0x3e(%rsp)
   1297a:	je     12c40 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x310>
   12980:	mov    0x58(%r15),%rbp
   12984:	cmp    0x50(%r15),%rbp
   12988:	je     12c70 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x340>
   1298e:	lea    -0x20(%rbp),%rax
   12992:	mov    %rax,0x30(%rsp)
   12997:	mov    -0x20(%rbp),%rax
   1299b:	cmp    %rax,0x28(%rsp)
   129a0:	mov    %rax,0x18(%rsp)
   129a5:	setle  %al
   129a8:	test   %al,%al
   129aa:	je     12c70 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x340>
   129b0:	mov    -0x8(%rbp),%r12d
   129b4:	test   %r12d,%r12d
   129b7:	je     12ab8 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x188>
   129bd:	mov    -0x10(%rbp),%eax
   129c0:	mov    0x28(%r15),%rdx
   129c4:	mov    (%r14),%rcx
   129c7:	lea    (%rax,%rax,2),%rbx
   129cb:	movq   %rax,%xmm0
   129d0:	shl    $0x4,%rbx
   129d4:	add    %rdx,%rbx
   129d7:	mov    0x18(%r14),%rax
   129db:	mov    0xc(%rsp),%edi
   129df:	lea    0x1(%rax),%rsi
   129e3:	lea    (%rax,%rax,4),%rax
   129e7:	lea    (%rcx,%rax,8),%rax
   129eb:	cmp    0x28(%rbx),%edi
   129ee:	jne    12ad8 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x1a8>
   129f4:	mov    0x8(%rbx),%rcx
   129f8:	mov    (%rbx),%rdi
   129fb:	mov    %rsi,0x18(%r14)
   129ff:	movzwl 0x1636a(%rip),%esi        # 28d70 <nanobook::latency::kAllTags+0x167>
   12a06:	movb   $0x0,0x2(%rax)
   12a0a:	mov    %rcx,0x20(%rax)
   12a0e:	mov    %si,(%rax)
   12a11:	mov    %rdi,0x8(%rax)
   12a15:	movq   $0x0,0x10(%rax)
   12a1d:	movq   $0x0,0x18(%rax)
   12a25:	mov    0x24(%rbx),%eax
   12a28:	sub    %rcx,-0x18(%rbp)
   12a2c:	mov    0x20(%rbx),%ecx
   12a2f:	cmp    $0xffffffff,%ecx
   12a32:	je     12c10 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x2e0>
   12a38:	mov    %ecx,%esi
   12a3a:	lea    (%rsi,%rsi,2),%rsi
   12a3e:	shl    $0x4,%rsi
   12a42:	mov    %eax,0x24(%rdx,%rsi,1)
   12a46:	mov    0x24(%rbx),%eax
   12a49:	cmp    $0xffffffff,%eax
   12a4c:	je     12c00 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x2d0>
   12a52:	lea    (%rax,%rax,2),%rax
   12a56:	shl    $0x4,%rax
   12a5a:	mov    %ecx,0x20(%rdx,%rax,1)
   12a5e:	mov    0x161bb(%rip),%rax        # 28c20 <nanobook::latency::kAllTags+0x17>
   12a65:	sub    $0x1,%r12d
   12a69:	mov    (%rbx),%rsi
   12a6c:	lea    0x88(%r15),%rdi
   12a73:	movd   %xmm0,0x10(%rsp)
   12a79:	mov    %rax,0x20(%rbx)
   12a7d:	mov    %r12d,-0x8(%rbp)
   12a81:	call   92c0 <nanobook::fast::IdTable::erase(nanobook::OrderId)>
   12a86:	mov    0x40(%r15),%eax
   12a8a:	movd   0x10(%rsp),%xmm0
   12a90:	movl   $0xfffffffe,0x20(%rbx)
   12a97:	mov    %eax,0x24(%rbx)
   12a9a:	mov    0x44(%r15),%eax
   12a9e:	sub    $0x1,%eax
   12aa1:	movd   %eax,%xmm4
   12aa5:	punpckldq %xmm4,%xmm0
   12aa9:	movq   %xmm0,0x40(%r15)
   12aaf:	test   %r12d,%r12d
   12ab2:	jne    129bd <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x8d>
   12ab8:	mov    0x30(%rsp),%rax
   12abd:	movzbl 0x3f(%rsp),%esi
   12ac2:	lea    0x48(%r15),%rdi
   12ac6:	mov    (%rax),%rdx
   12ac9:	call   9480 <nanobook::fast::SortedLevels::erase(nanobook::Side, nanobook::Price)>
   12ace:	jmp    1296c <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3c>
   12ad3:	nopl   0x0(%rax,%rax,1)
   12ad8:	mov    0x8(%rbx),%rdi
   12adc:	xor    %r8d,%r8d
   12adf:	cmp    %r13,%rdi
   12ae2:	jge    12aed <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x1bd>
   12ae4:	mov    %r13,%r8
   12ae7:	mov    %rdi,%r13
   12aea:	sub    %rdi,%r8
   12aed:	movzwl 0x16224(%rip),%edi        # 28d18 <nanobook::latency::kAllTags+0x10f>
   12af4:	movq   0x20(%rsp),%xmm2
   12afa:	movq   %r13,%xmm1
   12aff:	punpcklqdq %xmm1,%xmm1
   12b03:	movhps (%rbx),%xmm2
   12b06:	mov    %rsi,0x18(%r14)
   12b0a:	mov    %di,(%rax)
   12b0d:	mov    0x18(%rsp),%rdi
   12b12:	movups %xmm2,0x8(%rax)
   12b16:	movb   $0x0,0x2(%rax)
   12b1a:	mov    %rdi,0x18(%rax)
   12b1e:	mov    %r13,0x20(%rax)
   12b22:	movdqu 0x8(%rbx),%xmm2
   12b27:	movdqa %xmm2,%xmm3
   12b2b:	psubq  %xmm1,%xmm3
   12b2f:	paddq  %xmm2,%xmm1
   12b33:	movsd  %xmm3,%xmm1
   12b37:	movups %xmm1,0x8(%rbx)
   12b3b:	sub    %r13,-0x18(%rbp)
   12b3f:	cmpq   $0x0,0x8(%rbx)
   12b44:	je     12b60 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x230>
   12b46:	test   %r8,%r8
   12b49:	je     12c88 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x358>
   12b4f:	mov    %r8,%r13
   12b52:	jmp    129d7 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0xa7>
   12b57:	nopw   0x0(%rax,%rax,1)
   12b60:	mov    0x20(%rbx),%ecx
   12b63:	mov    0x24(%rbx),%eax
   12b66:	cmp    $0xffffffff,%ecx
   12b69:	je     12c30 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x300>
   12b6f:	mov    %ecx,%esi
   12b71:	lea    (%rsi,%rsi,2),%rsi
   12b75:	shl    $0x4,%rsi
   12b79:	mov    %eax,0x24(%rdx,%rsi,1)
   12b7d:	cmp    $0xffffffff,%eax
   12b80:	je     12c20 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x2f0>
   12b86:	lea    (%rax,%rax,2),%rax
   12b8a:	shl    $0x4,%rax
   12b8e:	mov    %ecx,0x20(%rdx,%rax,1)
   12b92:	mov    0x16087(%rip),%rax        # 28c20 <nanobook::latency::kAllTags+0x17>
   12b99:	sub    $0x1,%r12d
   12b9d:	mov    (%rbx),%rsi
   12ba0:	lea    0x88(%r15),%rdi
   12ba7:	mov    %r8,0x10(%rsp)
   12bac:	mov    %rax,0x20(%rbx)
   12bb0:	mov    %r12d,-0x8(%rbp)
   12bb4:	movd   %xmm0,0x38(%rsp)
   12bba:	call   92c0 <nanobook::fast::IdTable::erase(nanobook::OrderId)>
   12bbf:	mov    0x40(%r15),%eax
   12bc3:	movd   0x38(%rsp),%xmm0
   12bc9:	movl   $0xfffffffe,0x20(%rbx)
   12bd0:	mov    0x10(%rsp),%r8
   12bd5:	mov    %eax,0x24(%rbx)
   12bd8:	mov    0x44(%r15),%eax
   12bdc:	sub    $0x1,%eax
   12bdf:	movd   %eax,%xmm5
   12be3:	punpckldq %xmm5,%xmm0
   12be7:	movq   %xmm0,0x40(%r15)
   12bed:	test   %r8,%r8
   12bf0:	je     12c88 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x358>
   12bf6:	mov    %r8,%r13
   12bf9:	jmp    129b4 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x84>
   12bfe:	xchg   %ax,%ax
   12c00:	mov    %ecx,-0xc(%rbp)
   12c03:	jmp    12a5e <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x12e>
   12c08:	nopl   0x0(%rax,%rax,1)
   12c10:	mov    %eax,-0x10(%rbp)
   12c13:	jmp    12a49 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x119>
   12c18:	nopl   0x0(%rax,%rax,1)
   12c20:	mov    %ecx,-0xc(%rbp)
   12c23:	jmp    12b92 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x262>
   12c28:	nopl   0x0(%rax,%rax,1)
   12c30:	mov    %eax,-0x10(%rbp)
   12c33:	jmp    12b7d <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x24d>
   12c38:	nopl   0x0(%rax,%rax,1)
   12c40:	mov    0x78(%r15),%rbp
   12c44:	cmp    0x70(%r15),%rbp
   12c48:	je     12c70 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x340>
   12c4a:	lea    -0x20(%rbp),%rax
   12c4e:	mov    %rax,0x30(%rsp)
   12c53:	mov    -0x20(%rbp),%rax
   12c57:	cmp    %rax,0x28(%rsp)
   12c5c:	mov    %rax,0x18(%rsp)
   12c61:	setge  %al
   12c64:	test   %al,%al
   12c66:	jne    129b0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x80>
   12c6c:	nopl   0x0(%rax)
   12c70:	mov    %r13,%r14
   12c73:	add    $0x48,%rsp
   12c77:	mov    %r14,%rax
   12c7a:	pop    %rbx
   12c7b:	pop    %rbp
   12c7c:	pop    %r12
   12c7e:	pop    %r13
   12c80:	pop    %r14
   12c82:	pop    %r15
   12c84:	ret
   12c85:	nopl   (%rax)
   12c88:	test   %r12d,%r12d
   12c8b:	je     12c92 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x362>
   12c8d:	xor    %r14d,%r14d
   12c90:	jmp    12c73 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x343>
   12c92:	mov    0x30(%rsp),%rax
   12c97:	movzbl 0x3f(%rsp),%esi
   12c9c:	lea    0x48(%r15),%rdi
   12ca0:	mov    (%rax),%rdx
   12ca3:	call   9480 <nanobook::fast::SortedLevels::erase(nanobook::Side, nanobook::Price)>
   12ca8:	jmp    12c8d <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x35d>
   12caa:	cs nopw 0x0(%rax,%rax,1)
   12cb4:	cs nopw 0x0(%rax,%rax,1)
   12cbe:	xchg   %ax,%ax
