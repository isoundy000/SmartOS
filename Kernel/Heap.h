﻿#ifndef __Heap_H__
#define __Heap_H__

// 堆管理
class Heap
{
public:
	uint	Address;// 开始地址
	uint	Size;	// 大小

	Heap(uint addr, uint size);
	
	uint Used() const;	// 已使用内存数
	uint Count() const;	// 已使用内存块数
	
	void* Alloc(uint size);
	void Free(void* ptr);
	
private:
	uint	_Used;
	uint	_Count;
};

#endif
