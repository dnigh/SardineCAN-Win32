/*
**
** Copyright (C) 2012 Olaf @ Hacking Volvo blog (hackingvolvo.blogspot.com)
** Author: Olaf <hackingvolvo@gmail.com>
**
** This library is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published
** by the Free Software Foundation, either version 3 of the License, or (at
** your option) any later version.
**
** This library is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public
** License along with this library; if not, <http://www.gnu.org/licenses/>.
**
*/

#include "stdafx.h"
#include "Arduino.h"
#include "helper.h"
#include <stdio.h>
#include "Benaphore.h"
#include "comm.h"
#include <WinSock.h>

#define MAX_COLLECTION_BUF_SIZE 1024
#define MAX_READ_BUF_SIZE 256
#define MAX_LISTENERS 8

namespace Arduino {


	Benaphore init_wait_lock;

	Benaphore init_lock;
	Benaphore listener_lock;

	Benaphore write_lock;

	bool isConnected=0;

	HANDLE hCommPort = INVALID_HANDLE_VALUE;

	OVERLAPPED read_overlap;
	OVERLAPPED write_overlap;
	OVERLAPPED comm_event_overlap;

	HANDLE ghWriteCompleteEvent;

	DWORD dwCommEventMask;	

	typedef struct {
		LPARDUINOLISTENER callback;
		void * data;		
	} listener_struct;

	listener_struct listeners[MAX_LISTENERS];
	int listeners_count=0;

	char collectionbuf[MAX_COLLECTION_BUF_SIZE+1];
	char buffer[MAX_READ_BUF_SIZE];
	int cbufi=0;


int RegisterListener( LPARDUINOLISTENER listener, void *data )
	{
	LOG(MAINFUNC,"Arduino::RegisterListener");
	listener_lock.Lock();
	if (listeners_count<MAX_LISTENERS)
		{
		listeners[listeners_count].callback = listener;
		listeners[listeners_count].data = data;
		listeners_count++;
		listener_lock.Unlock();
		LOG(MAINFUNC,"Arduino::RegisterListener - added succesfully");
		}
	else
		{
		listener_lock.Unlock();
		LOG(ERR,"Arduino::RegisterListener - too many listeners!");
		return -1;
		}
	return 0;
	}

void RemoveListener( LPARDUINOLISTENER listener )
	{
	LOG(MAINFUNC,"Arduino::RemoveListener");
	listener_lock.Lock();
	int i=0;
	while ( (i<listeners_count) && (listeners[i].callback!=listener) )
		i++;
	if (listeners[i].callback==listener)
		{
		// remove entry so that entries after this are simply moved 1 entry lower
		while (i<listeners_count-1)
			{
			listeners[i].callback = listeners[i+1].callback;
			listeners[i].data = listeners[i+1].data;
			i++;
			}
		listeners_count--;
		listener_lock.Unlock();
		LOG(MAINFUNC,"Arduino::RemoveListener - removed successfully");
		}
	else
		{
		listener_lock.Unlock();
		LOG(ERR,"Arduino::RemoveListener - no such listener found!");
		}
	}


bool IsConnected()
{
	init_lock.Lock();
	LOG(HELPERFUNC,"Arduino::IsConnected %d",isConnected);
	bool isconnected = isConnected;
	init_lock.Unlock();
	return isconnected;
}


int blockingWrite( char * buf, unsigned int len )
{
	DWORD dwwritten = 0, dwErr;
	memset(&write_overlap, 0, sizeof(write_overlap));
	write_overlap.hEvent = ghWriteCompleteEvent; // CreateEvent(NULL, FALSE, FALSE, NULL); // TRUE, NULL); // for some reason event 
	unsigned int fSuccess = WriteFile(hCommPort, buf, len, &dwwritten, &write_overlap);
	if (!fSuccess) 
	{
		dwErr = GetLastError();
		if (dwErr != ERROR_IO_PENDING)
			{
			LOG(ERR,"Arduino::SendMsg - Write failed (%d)\n", GetLastError());
			return -1;
			}

		// Pending IO -> Wait for the result
	if (!GetOverlappedResult(hCommPort, &write_overlap, &dwwritten, TRUE))
		{
		LOG(ERR,"Arduino::SendMsg - Error waiting for write to finish (%d)\n", GetLastError());
		return -1;
		}

	} 
	
	return dwwritten;
}


int OpenDevice( int com_port, int baud_rate, int disable_DTR )
{
	LOG(MAINFUNC,"Arduino::OpenDevice - com port %d, baud_rate %d, disable DTR %d",com_port,baud_rate,disable_DTR);
	init_lock.Lock();
	if (isConnected)
		{
		init_lock.Unlock();
		LOG(ERR,"Arduino::OpenDevice - already connected!");
		return ARDUINO_ALREADY_CONNECTED;
		}

	if (com_port==-1)
		{
		com_port = ARDUINO_DEFAULT_COM_PORT;
		LOG(MAINFUNC,"Arduino::OpenDevice - defaulting to comport %d",com_port);
		}
	if (baud_rate==-1)
		{
		baud_rate = ARDUINO_DEFAULT_BAUD_RATE;
		LOG(MAINFUNC,"Arduino::OpenDevice - defaulting to baud rate %d",baud_rate);
		}
	if (disable_DTR==-1)
		{
		disable_DTR = ARDUINO_DEFAULT_DTR_DISABLED;
		LOG(MAINFUNC,"Arduino::OpenDevice - defaulting to dtr disabled=%d",disable_DTR);
		}
	

	TCHAR COMx[32];
	int n = _stprintf_s(COMx, 32, _T("\\\\.\\COM%d"),com_port);
	long int err;
	BOOL fSuccess;	
	DCB dcb;

	LOG(MAINFUNC,"Arduino::OpenDevice - creating file handle for com port");
	hCommPort = CreateFile(
	COMx,
    GENERIC_READ | GENERIC_WRITE,
    FILE_SHARE_WRITE, // 0,
    NULL,
    OPEN_EXISTING,
    FILE_FLAG_OVERLAPPED,
    NULL
    );

	if (hCommPort == INVALID_HANDLE_VALUE) 
	{
		err=GetLastError();
		if (err==ERROR_ALREADY_EXISTS)
			{
			LOG(ERR,"Arduino::OpenDevice - Already opened by other process!");
			init_lock.Unlock();
			return ARDUINO_IN_USE;
			} 
		else
			{
			LOG(ERR,"Arduino::OpenDevice - CreateFileA failed %d!",err);
			init_lock.Unlock();
			return ARDUINO_OPEN_FAILED;
			}
	}

	LOG(MAINFUNC,"Arduino::OpenDevice - getting comm state");
	fSuccess = GetCommState(hCommPort, &dcb);

	if (!fSuccess) 
	{
		LOG(ERR,"Arduino::OpenDevice - GetCommStateFailed %d", err=GetLastError());
		PrintError(err);
		CloseHandle(hCommPort);
		hCommPort=INVALID_HANDLE_VALUE;
		init_lock.Unlock();
		return ARDUINO_GET_COMMSTATE_FAILED;
	}

	LOG(MAINFUNC,"Arduino::OpenDevice - setting comm state");
	dcb.BaudRate = baud_rate;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fDtrControl = (disable_DTR==1 ? DTR_CONTROL_DISABLE : DTR_CONTROL_ENABLE);	// to prevent Arduino from reseting when first sending something
		
	fSuccess = SetCommState(hCommPort, &dcb);
	if (!fSuccess) 
	{
		err=GetLastError();
		LOG(ERR,"Arduino::OpenDevice - SetCommStateFailed %d", err);
		CloseHandle(hCommPort);
		hCommPort=INVALID_HANDLE_VALUE;
		PrintError(err);
		init_lock.Unlock();
		return ARDUINO_SET_COMMSTATE_FAILED;
	}


	LOG(MAINFUNC,"Arduino::OpenDevice - enable listening to comm events");
	DWORD      dwStoredFlags;
	// listen only for events regarding errors and receive buffer
	dwStoredFlags = /*EV_BREAK | EV_CTS |*/ EV_DSR | /* EV_ERR | EV_RING | EV_RLSD | */ EV_RXCHAR /* | EV_RXFLAG | EV_TXEMPTY*/ ;
	if (!SetCommMask(hCommPort, dwStoredFlags))
	{
		err=GetLastError();
		LOG(ERR,"Arduino::OpenDevice - Error setting communications mask (err %d); abort!", err);
		CloseHandle(hCommPort);
		hCommPort=INVALID_HANDLE_VALUE;
		PrintError(err);
		init_lock.Unlock();
		return ARDUINO_SET_COMMMASK_FAILED;
	}

	// create event for write completed
	if ((ghWriteCompleteEvent = CreateEvent(NULL, FALSE, FALSE, NULL)) == NULL)
	{
		LOG(ERR,"Arduino::OpenDevice - Create 'Write Complete Event' failed (err %d); abort!", GetLastError());
		return ARDUINO_CREATE_EVENT_FAILED;
	}


	blockingWrite("\r\r\r",3);

	isConnected=true;

#ifdef USBCAN_PROTOCOL
	Send("Z0");	// we are not interested in time stamps, we generate our own
#else
	Send(":ping");
#endif

	init_lock.Unlock();
	LOG(MAINFUNC,"Arduino::OpenDevice - port configured");
	return ARDUINO_INIT_OK;
}


int CloseDevice()
{
	LOG(MAINFUNC,"Arduino::CloseDevice");
	init_lock.Lock();
	CloseHandle(hCommPort);
	hCommPort=INVALID_HANDLE_VALUE;
	isConnected=0;
	CloseHandle(ghWriteCompleteEvent);
	init_lock.Unlock();
	return 0;
}


int Send(const char * cmd)
	{
	LOG(ARDUINO_MSG_VERBOSE,"Arduino::SendMsg - msg [%s]",cmd);
	write_lock.Lock();

	DWORD dwwritten = 0; 
	int len = strlen(cmd);

	char buf[256];
	if(len>256)
	{
		write_lock.Unlock();
		LOG(ERR,"Arduino::Send: Msg too long!");
		return -1;
	}

#ifndef USBCAN_PROTOCOL
	// surround message with {}
	sprintf_s(buf,256,"{%s}\r",cmd);
	len += 3;	// {}\r
#else
	sprintf_s(buf,256,"%s\r",cmd);
	len += 1;	// \r
#endif

	if  ( (dwwritten=blockingWrite(buf, len )) == -1)
		{
		LOG(ERR,"Arduino::SendMsg - Blocking write failed ! \n");
		write_lock.Unlock();
		return -1;
		}

	if (dwwritten != len)
		{
		write_lock.Unlock();
		LOG(ERR,"Arduino::SendMsg - Write didn't finish (%d out of %d bytes sent)\n", dwwritten,len);
		DWORD   dwErrors;
		COMSTAT comStat;
		ClearCommError(hCommPort, &dwErrors, &comStat);
		LOG(ERR,"Arduino::SendMsg - ClearCommError: Error flags: 0x%x, bytes in output queue: %d\n", dwErrors, comStat.cbOutQue);		
		return -1;
		}
	write_lock.Unlock();

#ifndef USBCAN_PROTOCOL
	LOG(ARDUINO_MSG_VERBOSE,"Arduino::SendMsg - completed succefully: %d written (%d bytes original)",dwwritten,dwwritten-3);
	return dwwritten-3;
#else
	LOG(ARDUINO_MSG_VERBOSE,"Arduino::SendMsg - completed succefully: %d written (%d bytes original)",dwwritten,dwwritten-1);
	return dwwritten-1;
#endif
}




int Listen( HANDLE CommEventHandle )
{
	memset(&comm_event_overlap, 0, sizeof(comm_event_overlap));
	comm_event_overlap.hEvent = CommEventHandle; // comm event handle
	int ret;
//	if (!ReadFileEx(hCommPort, buffer,1, &read_overlap, (LPOVERLAPPED_COMPLETION_ROUTINE)&ReadRequestCompleted))
	ret=WaitCommEvent(hCommPort,&dwCommEventMask,&comm_event_overlap);
	if (!ret)
	{
		ret = GetLastError();
		if (ret != ERROR_IO_PENDING) 
		{
		LOG(ERR,"Arduino::WaitCommEvent error: %d",ret);
		return ret;
		}
	}
	return 0;
}


void MsgReceived( char * msg_buf, int len )
{
	LOG(ARDUINO_MSG,"Arduino::MsgReceived: read: %d bytes: [%s]",len,msg_buf);
//	LOG(ARDUINO_MSG_VERBOSE,"  Msg: [%s]",msg_buf);

#ifndef USBCAN_PROTOCOL
	if ( (msg_buf[0]=='{') && (msg_buf[len-1]=='}') )
	{
		// get rid of wavy brackets
		msg_buf++;
		len--;
		msg_buf[len-1]=0;
#endif

		// Callback for each listener
		int accepted=0;
		for (int i=0;i<listeners_count;i++)
		{
			if (listeners[i].callback(msg_buf,len,listeners[i].data))
				accepted++;
		}
		if (!accepted)
			{
			LOG(ERR,"Arduino::MsgReceived: Warning! None of the listeners accepted the message!");
			}
#ifndef USBCAN_PROTOCOL
	}
	else
	{
		LOG(ERR,"Arduino::MsgReceived: Msg not inside {} ! -- ignoring [%s]",msg_buf);
	}
#endif
}


// Parses received message, each separated by newline: Appends collectionbuf until newline is reached, then calls MsgReceived to let each listener handle message according
// to each protocol needs. Collectionbuf is then cleared and parsing can continue until there's no more bytes to read. If newline is not reached in the end, the chars received
// after previous newline will stay on collectionbuf to wait for next ReadRequestCompleted.
VOID CALLBACK ReadRequestCompleted( DWORD errorCode, DWORD bytesRead, LPVOID overlapped )
{
	buffer[bytesRead]=0;
	LOG(ARDUINO_MSG_VERBOSE,"Arduino::ReadRequestCompleted: errorCode %d, read: %d bytes: [%s]\n",errorCode,bytesRead,buffer);
	if (bytesRead>0)
	{
		unsigned int i=0;

		while (i<bytesRead) 
		{
			// handle one message  (messages separated by newline)
			while ((cbufi<MAX_COLLECTION_BUF_SIZE) && (i<bytesRead) && (buffer[i]!='\r') )
			{
				// line feeds are ignored
				if (buffer[i]!='\n')
					collectionbuf[cbufi++] = buffer[i++];
				else
					i++;
			}
			// If we have reached end of line, handle the message. However if collectionbuf has reached is maximum size, handle it as a whole message (shouldn't happen, but just in case). 
			// Collectionbuf is cleared in the end to make space for new messages
			if ( (buffer[i]=='\r') || (cbufi==MAX_COLLECTION_BUF_SIZE) )
				{
				collectionbuf[cbufi]=0;
				if (cbufi>0)
					MsgReceived(collectionbuf,cbufi);
				cbufi=0;
				}
			// ignore empty lines
			if ( (buffer[i]=='\n') || (buffer[i]=='\r') )
				i++;
		}
	 }
	// restart the read
//	ReadOperation();
}


int BlockingRead( int bytes )
{
	LOG(HELPERFUNC,"Arduino::BlockingRead: read %d bytes",bytes);
	//	if (!ReadFileEx(hCommPort, buffer,1, &read_overlap, (LPOVERLAPPED_COMPLETION_ROUTINE)&ReadRequestCompleted))
	DWORD err=0;
	DWORD bytesRead;
	DWORD bytesLeft = bytes;
	while ( (bytesLeft>0) && (!err) )
	{
		DWORD bytesToRead = ( bytesLeft>MAX_READ_BUF_SIZE ? MAX_READ_BUF_SIZE : bytesLeft );

		memset(&read_overlap, 0, sizeof(read_overlap));
		err=ReadFileEx(hCommPort, buffer,bytesToRead, &read_overlap,NULL);
		if (!err) 
		{
			err = GetLastError();
			if (err!=ERROR_IO_PENDING)
			{
				LOG(ERR,"Arduino::BlockingRead: ReadFileEx error! %d",err);
				return err;
			} else
				err=NULL; // omit ERROR_IO_PENDING since we want to continue reading
		}		
		if (!GetOverlappedResult(hCommPort, &read_overlap, &bytesRead, TRUE))
		{
			LOG(ERR,"Arduino::BlockingRead: GetOverlappedResult error! %d",err=GetLastError());
			return err;
		}
		if (bytesRead != bytes)
		{
			LOG(ERR,"Arduino::BlockingRead: bytes read (%d) != bytes requested (%d)! Still handling the bytes we got..",bytesRead,bytes);
		}
		ReadRequestCompleted(0,bytesRead,&read_overlap);
		bytesLeft -= bytesRead;
	}
	return STATUS_NOERROR;
}


int HandleCommEvent()
{
	// Get and clear current errors on the port.
	DWORD   dwErrors;
	COMSTAT comStat;
	DWORD ret;
	if (!ClearCommError(hCommPort, &dwErrors, &comStat))
	{
		LOG(ERR,"Arduino::HandleCommEvent - error calling ClearCommError: %d",ret=GetLastError());
		return ret;
	}
	if (dwErrors & CE_FRAME)
	{
		LOG(ERR,"Arduino::HandleCommEvent - hardware detected a framing error!");
	} 
	if (dwErrors & CE_OVERRUN)
	{
		LOG(ERR,"Arduino::HandleCommEvent - A character-buffer overrun has occurred. The next character is lost!");
	}
	if (dwErrors & CE_RXOVER)
	{
		LOG(ERR,"Arduino::HandleCommEvent - An input buffer overflow has occurred!");
	}
	if (dwErrors & CE_RXPARITY)
	{
		LOG(ERR,"Arduino::HandleCommEvent - hardware detected a parity error!");
	} 

	if (comStat.cbInQue>0)
	{
		LOG(HELPERFUNC,"Arduino::HandleCommEvent - %d bytes in receiving buffer!",comStat.cbInQue);
		return BlockingRead(comStat.cbInQue);
	}
	return 0;
} 

}