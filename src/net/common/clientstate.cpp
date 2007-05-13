/***************************************************************************
 *   Copyright (C) 2007 by Lothar May                                      *
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

#include <net/clientstate.h>
#include <net/clientthread.h>
#include <net/clientcontext.h>
#include <net/senderthread.h>
#include <net/receiverhelper.h>
#include <net/netpacket.h>
#include <net/resolverthread.h>
#include <net/clientexception.h>
#include <net/socket_helper.h>
#include <net/socket_msg.h>

using namespace std;

#define CLIENT_WAIT_TIMEOUT_MSEC	50


ClientState::~ClientState()
{
}

//-----------------------------------------------------------------------------

ClientStateInit &
ClientStateInit::Instance()
{
	static ClientStateInit state;
	return state;
}

ClientStateInit::ClientStateInit()
{
}

ClientStateInit::~ClientStateInit()
{
}

int
ClientStateInit::Process(ClientThread &client)
{
	ClientContext &context = client.GetContext();

	if (context.GetServerAddr().empty())
		throw ClientException(ERR_SOCK_SERVERADDR_NOT_SET, 0);

//	if (context.GetServerPort() < 1024)
//		throw ClientException(ERR_SOCK_INVALID_PORT, 0);

	context.SetSocket(socket(context.GetAddrFamily(), SOCK_STREAM, 0));
	if (!IS_VALID_SOCKET(context.GetSocket()))
		throw ClientException(ERR_SOCK_CREATION_FAILED, SOCKET_ERRNO());

	unsigned long mode = 1;
	if (IOCTLSOCKET(context.GetSocket(), FIONBIO, &mode) == SOCKET_ERROR)
		throw ClientException(ERR_SOCK_CREATION_FAILED, SOCKET_ERRNO());

	client.SetState(ClientStateStartResolve::Instance());

	return MSG_SOCK_INIT_DONE;
}

//-----------------------------------------------------------------------------

ClientStateStartResolve &
ClientStateStartResolve::Instance()
{
	static ClientStateStartResolve state;
	return state;
}

ClientStateStartResolve::ClientStateStartResolve()
{
}

ClientStateStartResolve::~ClientStateStartResolve()
{
}

int
ClientStateStartResolve::Process(ClientThread &client)
{
	int retVal;

	ClientContext &context = client.GetContext();

	context.GetClientSockaddr()->ss_family = context.GetAddrFamily();

	// Treat the server address as numbers first.
	if (socket_string_to_addr(
		context.GetServerAddr().c_str(),
		context.GetAddrFamily(),
		(struct sockaddr *)context.GetClientSockaddr(),
		context.GetClientSockaddrSize()))
	{
		// Success - but we still need to set the port.
		if (!socket_set_port(context.GetServerPort(), context.GetAddrFamily(), (struct sockaddr *)context.GetClientSockaddr(), context.GetClientSockaddrSize()))
			throw ClientException(ERR_SOCK_SET_PORT_FAILED, 0);

		// No need to resolve - start connecting.
		client.SetState(ClientStateStartConnect::Instance());
		retVal = MSG_SOCK_RESOLVE_DONE;
	}
	else
	{
		// Start name resolution in a separate thread, since it is blocking
		// for up to about 30 seconds.
		std::auto_ptr<ResolverThread> resolver(new ResolverThread);
		resolver->Init(context);
		resolver->Run();

		ClientStateResolving::Instance().SetResolver(resolver.release());
		client.SetState(ClientStateResolving::Instance());

		retVal = MSG_SOCK_INTERNAL_PENDING;
	}

	return retVal;
}

//-----------------------------------------------------------------------------

ClientStateResolving &
ClientStateResolving::Instance()
{
	static ClientStateResolving state;
	return state;
}

ClientStateResolving::ClientStateResolving()
: m_resolver(NULL)
{
}

ClientStateResolving::~ClientStateResolving()
{
	Cleanup();
}

void
ClientStateResolving::SetResolver(ResolverThread *resolver)
{
	Cleanup();
	m_resolver = resolver;
}

int
ClientStateResolving::Process(ClientThread &client)
{
	int retVal;

	if (!m_resolver)
		throw ClientException(ERR_SOCK_RESOLVE_FAILED, 0);

	if (m_resolver->Join(CLIENT_WAIT_TIMEOUT_MSEC))
	{
		ClientContext &context = client.GetContext();
		bool success = m_resolver->GetResult(context);
		Cleanup(); // Not required, but better keep things clean.

		if (!success)
			throw ClientException(ERR_SOCK_RESOLVE_FAILED, 0);

		client.SetState(ClientStateStartConnect::Instance());
		retVal = MSG_SOCK_RESOLVE_DONE;
	}
	else
		retVal = MSG_SOCK_INTERNAL_PENDING;

	return retVal;
}


void
ClientStateResolving::Cleanup()
{
	if (m_resolver)
	{
		if (m_resolver->Join(500))
			delete m_resolver;
		// If the resolver does not terminate fast enough, leave it
		// as memory leak.
		m_resolver = NULL;
	}
}

//-----------------------------------------------------------------------------

ClientStateStartConnect &
ClientStateStartConnect::Instance()
{
	static ClientStateStartConnect state;
	return state;
}

ClientStateStartConnect::ClientStateStartConnect()
{
}

ClientStateStartConnect::~ClientStateStartConnect()
{
}

int
ClientStateStartConnect::Process(ClientThread &client)
{
	int retVal;
	ClientContext &context = client.GetContext();

	int connectResult = connect(context.GetSocket(), (struct sockaddr *)context.GetClientSockaddr(), context.GetClientSockaddrSize());

	if (IS_VALID_CONNECT(connectResult))
	{
		client.SetState(ClientStateStartSession::Instance());
		retVal = MSG_SOCK_CONNECT_DONE;
	}
	else
	{
		int errCode = SOCKET_ERRNO();
		if (errCode == SOCKET_ERR_WOULDBLOCK)
		{
			client.SetState(ClientStateConnecting::Instance());
			retVal = MSG_SOCK_INTERNAL_PENDING;
		}
		else
			throw ClientException(ERR_SOCK_CONNECT_FAILED, SOCKET_ERRNO());
	}

	return retVal;
}

//-----------------------------------------------------------------------------

ClientStateConnecting &
ClientStateConnecting::Instance()
{
	static ClientStateConnecting state;
	return state;
}

ClientStateConnecting::ClientStateConnecting()
{
}

ClientStateConnecting::~ClientStateConnecting()
{
}

int
ClientStateConnecting::Process(ClientThread &client)
{
	int retVal;
	ClientContext &context = client.GetContext();

	fd_set writeSet;
	struct timeval timeout;

	FD_ZERO(&writeSet);
	FD_SET(context.GetSocket(), &writeSet);

	timeout.tv_sec  = 0;
	timeout.tv_usec = CLIENT_WAIT_TIMEOUT_MSEC * 1000;
	int selectResult = select(context.GetSocket() + 1, NULL, &writeSet, NULL, &timeout);

	if (selectResult > 0) // success
	{
		// Check whether the connect call succeeded.
		int connectResult = 0;
		socklen_t tmpSize = sizeof(connectResult);
		getsockopt(context.GetSocket(), SOL_SOCKET, SO_ERROR, (char *)&connectResult, &tmpSize);
		if (connectResult != 0)
			throw ClientException(ERR_SOCK_CONNECT_FAILED, connectResult);
		client.SetState(ClientStateStartSession::Instance());
		retVal = MSG_SOCK_CONNECT_DONE;
	}
	else if (selectResult == 0) // timeout
		retVal = MSG_SOCK_INTERNAL_PENDING;
	else
		throw ClientException(ERR_SOCK_SELECT_FAILED, SOCKET_ERRNO());


	return retVal;
}

//-----------------------------------------------------------------------------

ClientStateStartSession &
ClientStateStartSession::Instance()
{
	static ClientStateStartSession state;
	return state;
}

ClientStateStartSession::ClientStateStartSession()
{
}

ClientStateStartSession::~ClientStateStartSession()
{
}

int
ClientStateStartSession::Process(ClientThread &client)
{
	ClientContext &context = client.GetContext();

	NetPacketJoinGame::Data initData;
	initData.password = context.GetPassword();
	initData.playerName = context.GetPlayerName();
	initData.ptype = PLAYER_TYPE_HUMAN; // TODO

	boost::shared_ptr<NetPacket> packet(new NetPacketJoinGame);
	((NetPacketJoinGame *)packet.get())->SetData(initData);
	
	client.GetSender().Send(context.GetSocket(), packet);

	client.SetState(ClientStateWaitSession::Instance());

	return MSG_SOCK_INTERNAL_PENDING;
}

//-----------------------------------------------------------------------------

ClientStateWaitSession &
ClientStateWaitSession::Instance()
{
	static ClientStateWaitSession state;
	return state;
}

ClientStateWaitSession::ClientStateWaitSession()
{
}

ClientStateWaitSession::~ClientStateWaitSession()
{
}

int
ClientStateWaitSession::Process(ClientThread &client)
{
	int retVal = MSG_SOCK_INTERNAL_PENDING;
	ClientContext &context = client.GetContext();

	// delegate to receiver helper class

	boost::shared_ptr<NetPacket> tmpPacket = client.GetReceiver().Recv(context.GetSocket());

	if (tmpPacket.get())
	{
		if (tmpPacket->ToNetPacketJoinGameAck())
		{
			// Everything is fine - we joined the game.
			// Initialize game configuration.
			NetPacketJoinGameAck::Data joinGameAckData;
			tmpPacket->ToNetPacketJoinGameAck()->GetData(joinGameAckData);
			client.SetGameData(joinGameAckData.gameData);

			client.GetCallback().SignalNetClientPlayerJoined(context.GetPlayerName());

			client.SetState(ClientStateWaitGame::Instance());
			retVal = MSG_SOCK_SESSION_DONE;
		}
		else if (tmpPacket->ToNetPacketError())
		{
			// Server reported an error.
			NetPacketError::Data errorData;
			tmpPacket->ToNetPacketError()->GetData(errorData);
			// Show the error.
			throw ClientException(errorData.errorCode, 0);
		}
	}

	return retVal;
}

//-----------------------------------------------------------------------------

ClientStateWaitGame &
ClientStateWaitGame::Instance()
{
	static ClientStateWaitGame state;
	return state;
}

ClientStateWaitGame::ClientStateWaitGame()
{
}

ClientStateWaitGame::~ClientStateWaitGame()
{
}

int
ClientStateWaitGame::Process(ClientThread &client)
{
	int retVal = MSG_SOCK_INTERNAL_PENDING;
	ClientContext &context = client.GetContext();

	// delegate to receiver helper class
	boost::shared_ptr<NetPacket> tmpPacket = client.GetReceiver().Recv(context.GetSocket());

	if (tmpPacket.get())
	{
		if (tmpPacket->ToNetPacketGameStart())
		{
			client.SetState(ClientStateFinal::Instance());
			retVal = MSG_NET_GAME_CLIENT_START;
		}
		else if (tmpPacket->ToNetPacketPlayerJoined())
		{
			NetPacketPlayerJoined::Data playerData;
			tmpPacket->ToNetPacketPlayerJoined()->GetData(playerData);
			client.GetCallback().SignalNetClientPlayerJoined(playerData.playerName);
			client.GetPlayerMap()[playerData.playerId] = playerData.playerName;
		}
		else if (tmpPacket->ToNetPacketPlayerLeft())
		{
			// TODO hacked.
			NetPacketPlayerLeft::Data playerData;
			tmpPacket->ToNetPacketPlayerLeft()->GetData(playerData);
			client.GetCallback().SignalNetClientPlayerLeft(client.GetPlayerMap()[playerData.playerId]);
			client.GetPlayerMap().erase(playerData.playerId);
		}
	}
	// TODO: handle error packet

	return retVal;
}

//-----------------------------------------------------------------------------

ClientStateFinal &
ClientStateFinal::Instance()
{
	static ClientStateFinal state;
	return state;
}

ClientStateFinal::ClientStateFinal()
{
}

ClientStateFinal::~ClientStateFinal()
{
}

int
ClientStateFinal::Process(ClientThread &client)
{
	ClientContext &context = client.GetContext();

	// delegate to receiver helper class
	boost::shared_ptr<NetPacket> tmpPacket = client.GetReceiver().Recv(context.GetSocket());

	if (tmpPacket.get())
	{
		if (tmpPacket->ToNetPacketHandStart())
		{
			// TODO
			NetPacketHandStart::Data tmpData;
			tmpPacket->ToNetPacketHandStart()->GetData(tmpData);
			int z = 1;
		}
	}

	return MSG_SOCK_INTERNAL_PENDING;
}
