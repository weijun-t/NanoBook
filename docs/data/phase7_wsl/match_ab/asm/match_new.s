0000000000012c30 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)>:
   12c30:	endbr64
   12c34:	push   %r15
   12c36:	movq   %rsi,%xmm3
   12c3b:	mov    %r9,%r15
   12c3e:	push   %r14
   12c40:	push   %r13
   12c42:	push   %r12
   12c44:	push   %rbp
   12c45:	push   %rbx
   12c46:	mov    %rdi,%rbx
   12c49:	sub    $0x38,%rsp
   12c4d:	test   %cl,%cl
   12c4f:	movq   0x160f9(%rip),%xmm4        # 28d50 <nanobook::latency::kAllTags+0x147>
   12c57:	mov    %edx,0x28(%rsp)
   12c5b:	mov    0x70(%rsp),%r12
   12c60:	mov    %r8,0x20(%rsp)
   12c65:	mov    %cl,0x2e(%rsp)
   12c69:	sete   0x2f(%rsp)
   12c6e:	test   %r15,%r15
   12c71:	jle    12fdc <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3ac>
   12c77:	cmpb   $0x0,0x2e(%rsp)
   12c7c:	je     12fb0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x380>
   12c82:	mov    0x58(%rbx),%rbp
   12c86:	cmp    %rbp,0x50(%rbx)
   12c8a:	je     12fdc <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3ac>
   12c90:	lea    -0x20(%rbp),%rax
   12c94:	mov    %rax,0x18(%rsp)
   12c99:	mov    -0x20(%rbp),%rax
   12c9d:	cmp    %rax,0x20(%rsp)
   12ca2:	mov    %rax,0x10(%rsp)
   12ca7:	setle  %al
   12caa:	test   %al,%al
   12cac:	je     12fdc <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3ac>
   12cb2:	mov    -0x8(%rbp),%r14d
   12cb6:	test   %r14d,%r14d
   12cb9:	je     12e86 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x256>
   12cbf:	mov    -0x10(%rbp),%eax
   12cc2:	mov    0x28(%rbx),%r8
   12cc6:	mov    (%r12),%rcx
   12cca:	lea    (%rax,%rax,2),%r10
   12cce:	mov    %rax,%r11
   12cd1:	shl    $0x4,%r10
   12cd5:	add    %r8,%r10
   12cd8:	mov    0x18(%r12),%rax
   12cdd:	mov    (%r10),%rsi
   12ce0:	mov    0x28(%rsp),%edi
   12ce4:	lea    0x1(%rax),%rdx
   12ce8:	lea    (%rax,%rax,4),%rax
   12cec:	lea    (%rcx,%rax,8),%rax
   12cf0:	cmp    0x28(%r10),%edi
   12cf4:	jne    12eb8 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x288>
   12cfa:	mov    0x8(%r10),%rcx
   12cfe:	movzwl 0x1607b(%rip),%edi        # 28d80 <nanobook::latency::kAllTags+0x177>
   12d05:	mov    %rdx,0x18(%r12)
   12d0a:	mov    0x20(%r10),%edx
   12d0e:	movb   $0x0,0x2(%rax)
   12d12:	mov    %di,(%rax)
   12d15:	mov    %rsi,0x8(%rax)
   12d19:	movq   $0x0,0x10(%rax)
   12d21:	movq   $0x0,0x18(%rax)
   12d29:	mov    %rcx,0x20(%rax)
   12d2d:	mov    0x24(%r10),%eax
   12d31:	sub    %rcx,-0x18(%rbp)
   12d35:	cmp    $0xffffffff,%edx
   12d38:	je     12fa0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x370>
   12d3e:	mov    %edx,%ecx
   12d40:	lea    (%rcx,%rcx,2),%rcx
   12d44:	shl    $0x4,%rcx
   12d48:	mov    %eax,0x24(%r8,%rcx,1)
   12d4d:	cmp    $0xffffffff,%eax
   12d50:	je     12f90 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x360>
   12d56:	lea    (%rax,%rax,2),%rax
   12d5a:	shl    $0x4,%rax
   12d5e:	mov    %edx,0x20(%r8,%rax,1)
   12d63:	mov    0xb0(%rbx),%ecx
   12d69:	sub    $0x1,%r14d
   12d6d:	movabs $0x9e3779b97f4a7c15,%rdx
   12d77:	mov    %r10,0x8(%rsp)
   12d7c:	imul   %rsi,%rdx
   12d80:	mov    %r14d,-0x8(%rbp)
   12d84:	mov    0x90(%rbx),%r9
   12d8b:	mov    0xa8(%rbx),%r13
   12d92:	mov    %rdx,%rax
   12d95:	shr    %cl,%rdx
   12d98:	shr    $0x20,%rax
   12d9c:	mov    %eax,%edi
   12d9e:	jmp    12da7 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x177>
   12da0:	add    $0x1,%rdx
   12da4:	and    %r13,%rdx
   12da7:	lea    (%r9,%rdx,8),%rax
   12dab:	cmp    (%rax),%edi
   12dad:	jne    12da0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x170>
   12daf:	mov    0x4(%rax),%r10d
   12db3:	lea    (%r10,%r10,2),%r10
   12db7:	shl    $0x4,%r10
   12dbb:	cmp    %rsi,(%r8,%r10,1)
   12dbf:	jne    12da0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x170>
   12dc1:	lea    0x1(%rdx),%rsi
   12dc5:	sub    $0x20,%ecx
   12dc8:	mov    0x8(%rsp),%r10
   12dcd:	and    %r13,%rsi
   12dd0:	lea    (%r9,%rsi,8),%rdi
   12dd4:	cmpl   $0xffffffff,0x4(%rdi)
   12dd8:	je     12e4a <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x21a>
   12dda:	mov    %r10,0x8(%rsp)
   12ddf:	nop
   12de0:	mov    (%rdi),%eax
   12de2:	shr    %cl,%eax
   12de4:	cmp    %rax,%rdx
   12de7:	setb   %r8b
   12deb:	cmp    %rax,%rsi
   12dee:	setae  %al
   12df1:	mov    %r8d,%r10d
   12df4:	and    %eax,%r10d
   12df7:	or     %r8d,%eax
   12dfa:	cmp    %rdx,%rsi
   12dfd:	cmovae %r10d,%eax
   12e01:	test   %al,%al
   12e03:	jne    12e30 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x200>
   12e05:	mov    (%rdi),%rax
   12e08:	mov    %rax,(%r9,%rdx,8)
   12e0c:	lea    0x1(%rsi),%rax
   12e10:	and    %r13,%rax
   12e13:	lea    (%r9,%rax,8),%rdi
   12e17:	cmpl   $0xffffffff,0x4(%rdi)
   12e1b:	je     12f80 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x350>
   12e21:	mov    %rsi,%rdx
   12e24:	mov    %rax,%rsi
   12e27:	jmp    12de0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x1b0>
   12e29:	nopl   0x0(%rax)
   12e30:	add    $0x1,%rsi
   12e34:	and    %r13,%rsi
   12e37:	lea    (%r9,%rsi,8),%rdi
   12e3b:	cmpl   $0xffffffff,0x4(%rdi)
   12e3f:	jne    12de0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x1b0>
   12e41:	mov    0x8(%rsp),%r10
   12e46:	lea    (%r9,%rdx,8),%rax
   12e4a:	movq   %xmm4,(%rax)
   12e4e:	mov    0x40(%rbx),%eax
   12e51:	movd   %r11d,%xmm0
   12e56:	subq   $0x1,0xb8(%rbx)
   12e5e:	mov    %eax,0x24(%r10)
   12e62:	mov    0x44(%rbx),%eax
   12e65:	movl   $0xfffffffe,0x20(%r10)
   12e6d:	sub    $0x1,%eax
   12e70:	movd   %eax,%xmm5
   12e74:	punpckldq %xmm5,%xmm0
   12e78:	movq   %xmm0,0x40(%rbx)
   12e7d:	test   %r14d,%r14d
   12e80:	jne    12cbf <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x8f>
   12e86:	mov    0x18(%rsp),%rax
   12e8b:	movzbl 0x2f(%rsp),%esi
   12e90:	lea    0x48(%rbx),%rdi
   12e94:	movq   %xmm3,0x8(%rsp)
   12e9a:	mov    (%rax),%rdx
   12e9d:	call   9340 <nanobook::fast::SortedLevels::erase(nanobook::Side, nanobook::Price)>
   12ea2:	movq   0x8(%rsp),%xmm3
   12ea8:	movq   0x15ea0(%rip),%xmm4        # 28d50 <nanobook::latency::kAllTags+0x147>
   12eb0:	jmp    12c6e <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3e>
   12eb5:	nopl   (%rax)
   12eb8:	mov    0x8(%r10),%rdi
   12ebc:	xor    %r13d,%r13d
   12ebf:	cmp    %r15,%rdi
   12ec2:	jge    12ecd <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x29d>
   12ec4:	sub    %rdi,%r15
   12ec7:	mov    %r15,%r13
   12eca:	mov    %rdi,%r15
   12ecd:	movzwl 0x15e44(%rip),%edi        # 28d18 <nanobook::latency::kAllTags+0x10f>
   12ed4:	mov    %rdx,0x18(%r12)
   12ed9:	movdqa %xmm3,%xmm1
   12edd:	movq   %rsi,%xmm6
   12ee2:	punpcklqdq %xmm6,%xmm1
   12ee6:	movb   $0x0,0x2(%rax)
   12eea:	movq   %r15,%xmm0
   12eef:	mov    %di,(%rax)
   12ef2:	mov    0x10(%rsp),%rdi
   12ef7:	punpcklqdq %xmm0,%xmm0
   12efb:	movups %xmm1,0x8(%rax)
   12eff:	mov    %rdi,0x18(%rax)
   12f03:	mov    %r15,0x20(%rax)
   12f07:	movdqu 0x8(%r10),%xmm1
   12f0d:	movdqa %xmm1,%xmm2
   12f11:	psubq  %xmm0,%xmm2
   12f15:	paddq  %xmm1,%xmm0
   12f19:	movsd  %xmm2,%xmm0
   12f1d:	movups %xmm0,0x8(%r10)
   12f22:	sub    %r15,-0x18(%rbp)
   12f26:	cmpq   $0x0,0x8(%r10)
   12f2b:	je     12f40 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x310>
   12f2d:	test   %r13,%r13
   12f30:	je     12ff0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3c0>
   12f36:	mov    %r13,%r15
   12f39:	jmp    12cd8 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0xa8>
   12f3e:	xchg   %ax,%ax
   12f40:	mov    0x18(%rsp),%rsi
   12f45:	mov    %r11d,%edx
   12f48:	mov    %rbx,%rdi
   12f4b:	movq   %xmm3,0x8(%rsp)
   12f51:	call   12aa0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::remove_resting(nanobook::fast::Level&, unsigned int)>
   12f56:	mov    -0x8(%rbp),%r14d
   12f5a:	test   %r13,%r13
   12f5d:	je     12ff0 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3c0>
   12f63:	movq   0x15de5(%rip),%xmm4        # 28d50 <nanobook::latency::kAllTags+0x147>
   12f6b:	movq   0x8(%rsp),%xmm3
   12f71:	mov    %r13,%r15
   12f74:	jmp    12cb6 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x86>
   12f79:	nopl   0x0(%rax)
   12f80:	mov    0x8(%rsp),%r10
   12f85:	mov    %rsi,%rdx
   12f88:	jmp    12e46 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x216>
   12f8d:	nopl   (%rax)
   12f90:	mov    %edx,-0xc(%rbp)
   12f93:	jmp    12d63 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x133>
   12f98:	nopl   0x0(%rax,%rax,1)
   12fa0:	mov    %eax,-0x10(%rbp)
   12fa3:	jmp    12d4d <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x11d>
   12fa8:	nopl   0x0(%rax,%rax,1)
   12fb0:	mov    0x78(%rbx),%rbp
   12fb4:	cmp    0x70(%rbx),%rbp
   12fb8:	je     12fdc <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3ac>
   12fba:	lea    -0x20(%rbp),%rax
   12fbe:	mov    %rax,0x18(%rsp)
   12fc3:	mov    -0x20(%rbp),%rax
   12fc7:	cmp    %rax,0x20(%rsp)
   12fcc:	mov    %rax,0x10(%rsp)
   12fd1:	setge  %al
   12fd4:	test   %al,%al
   12fd6:	jne    12cb2 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x82>
   12fdc:	add    $0x38,%rsp
   12fe0:	mov    %r15,%rax
   12fe3:	pop    %rbx
   12fe4:	pop    %rbp
   12fe5:	pop    %r12
   12fe7:	pop    %r13
   12fe9:	pop    %r14
   12feb:	pop    %r15
   12fed:	ret
   12fee:	xchg   %ax,%ax
   12ff0:	test   %r14d,%r14d
   12ff3:	je     12ffa <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3ca>
   12ff5:	xor    %r15d,%r15d
   12ff8:	jmp    12fdc <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3ac>
   12ffa:	mov    0x18(%rsp),%rax
   12fff:	movzbl 0x2f(%rsp),%esi
   13004:	lea    0x48(%rbx),%rdi
   13008:	mov    (%rax),%rdx
   1300b:	call   9340 <nanobook::fast::SortedLevels::erase(nanobook::Side, nanobook::Price)>
   13010:	jmp    12ff5 <nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>::match(nanobook::OrderId, nanobook::OwnerId, nanobook::Side, nanobook::Price, nanobook::Qty, nanobook::EventBuffer&)+0x3c5>
   13012:	cs nopw 0x0(%rax,%rax,1)
   1301c:	nopl   0x0(%rax)
