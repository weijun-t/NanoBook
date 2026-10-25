00000000000092c0 <nanobook::fast::IdTable::erase(nanobook::OrderId)>:
    92c0:	endbr64
    92c4:	movabs $0x9e3779b97f4a7c15,%rdx
    92ce:	push   %rbp
    92cf:	mov    %rdi,%r10
    92d2:	imul   %rsi,%rdx
    92d6:	push   %rbx
    92d7:	mov    0x28(%rdi),%ecx
    92da:	mov    0x8(%rdi),%r9
    92de:	mov    0x20(%rdi),%r11
    92e2:	shr    %cl,%rdx
    92e5:	mov    %rdx,%rbp
    92e8:	shl    $0x4,%rbp
    92ec:	add    %r9,%rbp
    92ef:	cmp    0x0(%rbp),%rsi
    92f3:	jne    9380 <nanobook::fast::IdTable::erase(nanobook::OrderId)+0xc0>
    92f9:	movabs $0x9e3779b97f4a7c15,%rbx
    9303:	lea    0x1(%rdx),%rdi
    9307:	and    %r11,%rdi
    930a:	mov    %rdi,%r8
    930d:	shl    $0x4,%r8
    9311:	add    %r9,%r8
    9314:	mov    (%r8),%rax
    9317:	test   %rax,%rax
    931a:	je     93c5 <nanobook::fast::IdTable::erase(nanobook::OrderId)+0x105>
    9320:	imul   %rbx,%rax
    9324:	shr    %cl,%rax
    9327:	cmp    %rax,%rdx
    932a:	setb   %sil
    932e:	cmp    %rax,%rdi
    9331:	setae  %al
    9334:	mov    %esi,%ebp
    9336:	and    %eax,%ebp
    9338:	or     %eax,%esi
    933a:	cmp    %rdx,%rdi
    933d:	cmovae %ebp,%esi
    9340:	test   %sil,%sil
    9343:	jne    93a0 <nanobook::fast::IdTable::erase(nanobook::OrderId)+0xe0>
    9345:	mov    (%r8),%rax
    9348:	shl    $0x4,%rdx
    934c:	lea    0x1(%rdi),%rsi
    9350:	add    %r9,%rdx
    9353:	and    %r11,%rsi
    9356:	mov    %rax,(%rdx)
    9359:	mov    0x8(%r8),%eax
    935d:	mov    %rsi,%r8
    9360:	shl    $0x4,%r8
    9364:	add    %r9,%r8
    9367:	mov    %eax,0x8(%rdx)
    936a:	mov    %rdi,%rdx
    936d:	mov    (%r8),%rax
    9370:	test   %rax,%rax
    9373:	je     93bd <nanobook::fast::IdTable::erase(nanobook::OrderId)+0xfd>
    9375:	mov    %rsi,%rdi
    9378:	jmp    9320 <nanobook::fast::IdTable::erase(nanobook::OrderId)+0x60>
    937a:	nopw   0x0(%rax,%rax,1)
    9380:	add    $0x1,%rdx
    9384:	and    %r11,%rdx
    9387:	mov    %rdx,%rbp
    938a:	shl    $0x4,%rbp
    938e:	add    %r9,%rbp
    9391:	cmp    %rsi,0x0(%rbp)
    9395:	je     92f9 <nanobook::fast::IdTable::erase(nanobook::OrderId)+0x39>
    939b:	jmp    9380 <nanobook::fast::IdTable::erase(nanobook::OrderId)+0xc0>
    939d:	nopl   (%rax)
    93a0:	add    $0x1,%rdi
    93a4:	and    %r11,%rdi
    93a7:	mov    %rdi,%r8
    93aa:	shl    $0x4,%r8
    93ae:	add    %r9,%r8
    93b1:	mov    (%r8),%rax
    93b4:	test   %rax,%rax
    93b7:	jne    9320 <nanobook::fast::IdTable::erase(nanobook::OrderId)+0x60>
    93bd:	shl    $0x4,%rdx
    93c1:	lea    (%r9,%rdx,1),%rbp
    93c5:	movq   $0x0,0x0(%rbp)
    93cd:	movl   $0xffffffff,0x8(%rbp)
    93d4:	pop    %rbx
    93d5:	subq   $0x1,0x30(%r10)
    93da:	pop    %rbp
    93db:	ret
    93dc:	nopl   0x0(%rax)
