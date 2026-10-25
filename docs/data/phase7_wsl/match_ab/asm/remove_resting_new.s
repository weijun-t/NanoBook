0000000000012aa0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)>:
   12aa0:	endbr64
   12aa4:	mov    %edx,%edx
   12aa6:	push   %r12
   12aa8:	mov    %rdi,%r11
   12aab:	mov    %rsi,%rax
   12aae:	push   %rbp
   12aaf:	mov    %rdx,%rbp
   12ab2:	push   %rbx
   12ab3:	lea    (%rdx,%rdx,2),%rbx
   12ab7:	mov    0x28(%rdi),%r8
   12abb:	shl    $0x4,%rbx
   12abf:	add    %r8,%rbx
   12ac2:	mov    0x20(%rbx),%ecx
   12ac5:	mov    0x8(%rbx),%rdx
   12ac9:	sub    %rdx,0x8(%rsi)
   12acd:	mov    0x24(%rbx),%edx
   12ad0:	cmp    $0xffffffff,%ecx
   12ad3:	je     12c18 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0x178>
   12ad9:	mov    %ecx,%esi
   12adb:	lea    (%rsi,%rsi,2),%rsi
   12adf:	shl    $0x4,%rsi
   12ae3:	mov    %edx,0x24(%r8,%rsi,1)
   12ae8:	cmp    $0xffffffff,%edx
   12aeb:	je     12c24 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0x184>
   12af1:	lea    (%rdx,%rdx,2),%rdx
   12af5:	shl    $0x4,%rdx
   12af9:	mov    %ecx,0x20(%r8,%rdx,1)
   12afe:	mov    (%rbx),%r12
   12b01:	subl   $0x1,0x18(%rax)
   12b05:	movabs $0x9e3779b97f4a7c15,%rax
   12b0f:	mov    0x90(%r11),%r9
   12b16:	mov    0xa8(%r11),%r10
   12b1d:	imul   %r12,%rax
   12b21:	mov    %rax,%rcx
   12b24:	shr    $0x20,%rcx
   12b28:	mov    %ecx,%edi
   12b2a:	mov    0xb0(%r11),%ecx
   12b31:	shr    %cl,%rax
   12b34:	jmp    12b47 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0xa7>
   12b36:	cs nopw 0x0(%rax,%rax,1)
   12b40:	add    $0x1,%rax
   12b44:	and    %r10,%rax
   12b47:	lea    (%r9,%rax,8),%rdx
   12b4b:	cmp    (%rdx),%edi
   12b4d:	jne    12b40 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0xa0>
   12b4f:	mov    0x4(%rdx),%esi
   12b52:	lea    (%rsi,%rsi,2),%rsi
   12b56:	shl    $0x4,%rsi
   12b5a:	cmp    %r12,(%r8,%rsi,1)
   12b5e:	jne    12b40 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0xa0>
   12b60:	lea    0x1(%rax),%rsi
   12b64:	sub    $0x20,%ecx
   12b67:	and    %r10,%rsi
   12b6a:	lea    (%r9,%rsi,8),%r8
   12b6e:	cmpl   $0xffffffff,0x4(%r8)
   12b73:	je     12be6 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0x146>
   12b75:	data16 cs nopw 0x0(%rax,%rax,1)
   12b80:	mov    (%r8),%edi
   12b83:	shr    %cl,%edi
   12b85:	cmp    %rdi,%rax
   12b88:	setb   %dl
   12b8b:	cmp    %rdi,%rsi
   12b8e:	setae  %dil
   12b92:	mov    %edx,%r12d
   12b95:	and    %edi,%r12d
   12b98:	or     %edi,%edx
   12b9a:	cmp    %rax,%rsi
   12b9d:	cmovae %r12d,%edx
   12ba1:	test   %dl,%dl
   12ba3:	jne    12bd0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0x130>
   12ba5:	mov    (%r8),%rdx
   12ba8:	mov    %rdx,(%r9,%rax,8)
   12bac:	lea    0x1(%rsi),%rdx
   12bb0:	mov    %rsi,%rax
   12bb3:	and    %r10,%rdx
   12bb6:	lea    (%r9,%rdx,8),%r8
   12bba:	cmpl   $0xffffffff,0x4(%r8)
   12bbf:	je     12be2 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0x142>
   12bc1:	mov    %rdx,%rsi
   12bc4:	jmp    12b80 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0xe0>
   12bc6:	cs nopw 0x0(%rax,%rax,1)
   12bd0:	add    $0x1,%rsi
   12bd4:	and    %r10,%rsi
   12bd7:	lea    (%r9,%rsi,8),%r8
   12bdb:	cmpl   $0xffffffff,0x4(%r8)
   12be0:	jne    12b80 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0xe0>
   12be2:	lea    (%r9,%rax,8),%rdx
   12be6:	mov    0x16163(%rip),%rax        # 28d50 <nanobook::latency::kAllTags+0x147>
   12bed:	mov    %rax,(%rdx)
   12bf0:	mov    0x40(%r11),%eax
   12bf4:	subq   $0x1,0xb8(%r11)
   12bfc:	movl   $0xfffffffe,0x20(%rbx)
   12c03:	mov    %eax,0x24(%rbx)
   12c06:	pop    %rbx
   12c07:	mov    %ebp,0x40(%r11)
   12c0b:	pop    %rbp
   12c0c:	subl   $0x1,0x44(%r11)
   12c11:	pop    %r12
   12c13:	ret
   12c14:	nopl   0x0(%rax)
   12c18:	mov    %edx,0x10(%rsi)
   12c1b:	cmp    $0xffffffff,%edx
   12c1e:	jne    12af1 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0x51>
   12c24:	mov    %ecx,0x14(%rax)
   12c27:	jmp    12afe <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)+0x5e>
   12c2c:	nopl   0x0(%rax)
