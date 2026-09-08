#ifndef __OMNIDECK_BOARD_H__
#define __OMNIDECK_BOARD_H__

/*
 * OmniDeckBoard 声明头 — 业务层 (omnideck_app / alarm_coordinator) 通过
 * OmniDeck() 取到板单例后即可访问 SHTC3 / PCF85063。
 * （实现在 omnideck.cc，DECLARE_BOARD 宏负责注册到 xiaozhi Board 工厂）
 */

#include "wifi_board.h"

class Shtc3;
class Pcf85063;

/* 业务层可见的板接口基类（具体实现在 omnideck.cc 中继承此类） */
class OmniDeckBoardBase : public WifiBoard {
public:
    virtual Shtc3& GetShtc3() = 0;
    virtual Pcf85063& GetRtc() = 0;
};

/* 便捷取用 */
OmniDeckBoardBase& OmniDeck();

#endif // __OMNIDECK_BOARD_H__
