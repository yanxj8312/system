TI_GDT equ 0
RPL0 equ 0
SELECTOR_VIDEO equ (0x0003<<3) + TI_GDT + RPL0

section .data
put_int_buffer dq 0 ;定义一个8字节缓冲区用于数字到字符的转换

[bits 32]
section .text
;---------------------------------------------------
;put_str通过put_char来打印以0字符结尾的字符串
;--------------------------------------------------
;输入：栈中参数为打印的字符串
;输出：无
global put_str
put_str:
;由于本函数中使用到ebx和ecx，只备份这两个寄存器
	push ebx
	push ecx
	xor ecx,ecx
	mov ebx, [esp + 12]
.goon:
	mov cl,[ebx]
	cmp cl, 0
	jz .str_over
	push ecx
	call put_char
	add esp, 4
	inc ebx
	jmp .goon
.str_over:
	pop ecx
	pop ebx
	ret






;--------------------put_char----------------------
;功能描述：把内存中的一个字符写入到光标所在处
;-------------------------------------------------
global put_char ;申明对外可见
put_char:
	pushad	;备份环境 备份8个双子长的寄存器
	;分别是eax,ecx,edx,ebx,esp,ebp,esi,edi
	;需要保证gs中为正确的视频段选择子
	;为保险起见，每次打印时都为gs赋值
	mov ax, SELECTOR_VIDEO;不能直接将立即数送入段寄存器
	mov gs,ax

;;;;;;;;;;;;;获取当前光标位置;;;;;;;;;;;;;;;;;;;;;;;;
	;先获得高8位
	mov dx, 0x03d4		;索引寄存器
	mov al, 0x0e		;用于提供光标位置的高8位
	out dx, al
	mov dx, 0x03d5		;通过读写数据端口0x3d5来获得或设置光标位置
	in al, dx		;得到光标位置的高8位
	mov ah, al

	;获得低8位
	mov dx, 0x03d4
	mov al, 0x0f
	out dx, al
	mov dx, 0x03d5
	in al,dx
	
	;将光标存入bx
	mov bx, ax
	;获取需要打印的字符
	mov ecx, [esp + 36] ;pushad压入4×8=32个字节，
				;加上主调函数4字节的返回地址，所以esp+36字节

	cmp cl,0xd	;CR是0x0d,LF是0x0a
	jz .is_carriage_return
	cmp cl, 0xa
	jz .is_line_feed

	cmp cl,0x8	;退格ascii是8
	jz .is_backspace
	jmp .put_other

.is_backspace:
;;;;;;;;;;;;;;;;backspace说明;;;;;;;;;;;;;;;;;;;;;;;
;当为backspace时，本质上只要将光标向前一个显存位即可，后面的
;再输入的字符自然会覆盖此处的字符，但是可能在键入backspace时候
;就不添加新的字符，那么这时候就需要消除回退的字符
	dec bx
	shl bx, 1	;光标左移一位等于乘以2
			;表示光标对应显存中的偏移字节
	mov byte [gs:bx], 0x20	;将待删除的字节补为0或者空格
	inc bx
	mov byte [gs:bx], 0x07
	shr bx,1
	jmp .set_cursor

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

.put_other:
	shl bx, 1	;光标位置用两个字节表示，将光标乘以2
			;表示对应现存中的偏移字节
	mov [gs:bx],cl	;ascii字符本身
	inc bx
	mov byte [gs:bx],0x07 ;字符属性
	shr bx, 1			;恢复老的光标值
	inc bx				;下一个光标值
	cmp bx, 2000		;若光标值小于2000表示未写到
	jl  .set_cursor	;显存的最后，则去设置新的光标值
			;若超出屏幕字符数大小则换行
.is_line_feed:
.is_carriage_return:
;如果是cr只要吧光标移动到行首就行
	xor dx,dx	;dx是被除数的高16位，清零
	mov ax,bx	;ax是被除数的低16位置
	mov si,80	;\r\n都被当成\n进行处理
	
	div si

	sub bx,dx

.is_carriage_return_end:
	add bx,80
	cmp bx,2000
.is_line_feed_end:
	jl .set_cursor

;屏幕行范围是0～24,滚屏的原理是将屏幕的1～24搬到0～23,将24用空格填充
.roll_screen:		;开始滚动屏幕
	cld
	mov ecx, 960

	mov esi, 0xc00b80a0
	mov edi, 0xc00b8000
	rep movsd

;将最后一行填充为空白
	mov ebx, 3840
	mov ecx, 80

.cls:
	mov word [gs:ebx], 0x0720
	add ebx,2
	loop .cls
	mov bx,1920	;光标重置为1920,最后一行的行首

.set_cursor:
;将光标设置为bx值
;;;;;;;;;;;;;;;;;先设置高8位;;;;;;;;;;;;;;;;;;
	mov dx, 0x03d4	;索引寄存器
	mov al, 0x0e	;用于提供光标位置的高8位
	out dx, al
	mov dx, 0x03d5	;通过0x03d5来获得光标的位置
	mov al, bh
	out dx, al

;;;;;;;;;;;;设置低8位置;;;;;;;;;;;;;;;;;;;;;;;
	mov dx, 0x03d4
	mov al, 0x0f
	out dx, al
	mov dx, 0x03d5
	mov al, bl
	out dx,al
.put_char_done:
	popad
	ret


;------------------------将小端字节序的数字变成对应的ascii后倒置---------
;输入：栈中参数为待打印的数字
;输出：在屏幕上打印十六进制数字，并不会打印前缀0x
;如打印十进制15时，只会直接打印f，不会打印0xf
;------------------------------------------------------------------------
global put_int
put_int:
	pushad
	mov ebp, esp 
	mov eax, [ebp + 4 * 9]	;call的返回地址占用4字节+pushad的8个4字节
	mov edx, eax
	mov edi, 7		;指定在put_int_buffer中初始的偏移量
	mov ecx, 8		;32位数字中，十六进制数字的位数是8个
	mov ebx, put_int_buffer 

;将32位数字按照十六进制的形式从低位到高位逐个处理
;共处理8个16进制数字
.16based_4bits:		;4个二进制代表一个十六进制
;便利每一个十六进制数字
	and edx, 0x0000000F	;解析十六进制数字的每一位
				;and操作后，edx只有低4位有效
	cmp edx, 9		;数字0～9和A～F需要分别处理成对应的字符
	jg .is_A2F		
	add edx,'0'		;ascii码是8位大小，add求和操作后，edx低8位有效
	jmp .store
.is_A2F:
	sub edx, 10		;A～F减去10所得到的差，再加上字符A的
				;ascii码，便是A～F对应的Ascii码
	add edx, 'A'
;将每一位数字转换成对应的字符后，按照类似‘大端’的顺序
;存储到缓冲区put_int_buffer
;高位字符放在低地址，低位字符放在高地址，这样和大端字节序类似
.store:
;此时dl中对应的应该是该数字对应字符的ascii码
	mov [ebx+edi], dl
	dec edi
	shr eax, 4
	mov edx, eax
	loop .16based_4bits

;现在put_int_buffer中已经全是字符，打印前需要吧高位字符去掉，
;比如0000123变成123
.ready_to_print:
	inc edi 		;此时edi退减为-1,加一使其为0
.skip_prefix_0:
	cmp edi, 8		;若已经比较第九个字符了，待打印字符为0

	je .full0
;找出连续的0字符，edi作为非0的最高位字符的偏移
.go_on_skip:
	mov cl, [put_int_buffer+edi]
	inc edi
	cmp cl, '0'
	je .skip_prefix_0	;判断下一个字符是否为字符0
	dec edi 		;edi在上面的inc操作中只想了下一个字符
	;若当前字符不为0,要使edi减1回复只想当前字符
	jmp .put_each_num

.full0:
	mov cl,'0'		;输入的数字全为0时，只打印0
.put_each_num:
	push ecx		;此时cl中为可打印的字符
	call put_char
	add esp, 4
	inc edi 		;使edi只想下一个字符
	mov cl, [put_int_buffer+edi]	;获取下一个字符到cl寄存器
	cmp edi ,8
	jl .put_each_num
	popad
	ret

global set_cursor
set_cursor:
	pushad
	mov bx, [esp + 36]		
;----------先设置高8位-----------
	mov dx, 0x03d4		;索引寄存器
	mov al, 0x0e		;用于提供光标位置的高8位
	out dx, al
	mov dx, 0x03d5		;通过读写数据端口0x3d5来获得或设置光标位置
	mov al, bh
	out dx, al

;------------设置低8位-------------
	mov dx, 0x03d4
	mov al, 0x0f
	out dx, al
	mov dx, 0x03d5
	mov al, bl
	out dx, al
	popad
	ret
