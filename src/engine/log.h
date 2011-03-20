/***************************************************************************
 *   Copyright (C) 2006 by FThauer FHammer   *
 *   f.thauer@web.de   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifndef LOG_H
#define LOG_H

#include "engine_defs.h"
#include "game_defs.h"

struct sqlite3;

class Log
{

public:
	Log(std::string logDirString, int logOnOffInt);

	~Log();

	void logNewGameMsg(int gameID, int startCash, int startSmallBlind, unsigned dealerPosition, PlayerList seatsList);
	void logNewHandMsg(int handID, unsigned dealerPosition, int smallBlind, unsigned smallBlindPosition, int bigBlind, unsigned bigBlindPosition, PlayerList seatsList);
	void logPlayerAction(int bero, int seat, PlayerActionLog action, int amount = 0);
	void logBoardCards(int bero, int boardCards[5]);
	void logHoleCards(int bero, int seat, PlayerActionLog action, int cards[2], int cardsValueInt = 0);
//	void Log::logFlipHoleCards(int bero, int seat, PlayerActionLog action, int cards[2], int cardsValueInt)
//    void closeLogDbAtExit();


private:
	sqlite3 *mySqliteLogDb;
	int curGameID;
	int curHandID;
	bool logOnOff;

};

#endif // LOG_H
