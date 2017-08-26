﻿#include "Kernel\TTime.h"
#include "Kernel\WaitHandle.h"

#include "Net\Socket.h"
#include "Net\NetworkInterface.h"

#include "Message\Json.h"

#include "LinkMessage.h"

#include "Security\RC4.h"

void LinkMessage::Init() {
	*(byte*)Reply = 0;
	Seq = 0;
	Code = 0;
	Length = 0;
}

const Json LinkMessage::Create() const { return Json((cstring)Data(), Length); }

// 在数据区上建立Json对象
Json LinkMessage::Create(int len) {
	len -= sizeof(this[0]);
	if (len <= 0) return Json();

	return Json((char*)Data(), len);
}

void LinkMessage::Show(bool newline) const {
	debug_printf("Seq=%d [%d] => ", Seq, Length);

	String str((cstring)&this[1], Length);
	str.Show(newline);
}