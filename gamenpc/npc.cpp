#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <sys/socket.h>
#include <netinet/tcp.h>

#include <string.h>
#include <stdlib.h>

#include <iostream>
#include <fstream>
#include <iterator>
#include <string>

#include "packet.h"
#include "sysdefine.h"
#include "alog.h"

#define DEFAULT_GAME_TIME 300
#define PAUSE_TIME 5

static struct event_base *base;
static std::vector<std::string> names;
static bool asNPC = true;
static struct timeval rematch_timeout;
static struct timeval reconnect_timeout;
static struct timeval sync_timeout;

class NPC{
public:
    NPC() {
        bev = NULL;
        rematch_timer = NULL;
        sync_timer = NULL;
        
        score = 0;
        hint = 5;
        bomb = 3;
        demon = 0;
        pause_time = 0;
    }
    const char *desc() { return  playerid.c_str(); }
    struct bufferevent *bev;
    struct event *rematch_timer;
    struct event *sync_timer;
    std::string playerid;
    std::string passwd;
    
    // game data
    unsigned int id_serial;
    time_t start_time;
    time_t game_time;
    time_t pause_time;
    unsigned int score;
    unsigned int hint;
    unsigned int bomb;
    unsigned int demon;
};

static void set_tcp_no_delay(evutil_socket_t fd)
{
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               &one, sizeof one);
}


// evbuffer stream interface
class EvBufferStream : public G4InStreamIF{
public:
    EvBufferStream(evbuffer *evbuff);
    virtual bool getbytes(unsigned char* buffer, unsigned short size);
    virtual bool skip(unsigned short offset);
private:
    evbuffer *evbuff;
};

EvBufferStream::EvBufferStream(evbuffer *evbuff){
    this->evbuff = evbuff;
}

bool EvBufferStream::getbytes(unsigned char* buffer, unsigned short size){
    if (evbuffer_get_length(evbuff) < (size_t)size)
        return false;
    evbuffer_remove(evbuff, buffer, (size_t)size);
    return true;
}

bool EvBufferStream::skip(unsigned short len){
    if (evbuffer_get_length(evbuff) < (size_t)len)
        return false;
    evbuffer_drain(evbuff, (size_t)len);
    return true;
}

static void match(NPC *npc)
{
    G4OutStream stream;
    G4NextPacket np;
    np._packetId = G4_COMM_MATCH_REQUEST;
    np.put32(G4_KEY_MIN_PLAYER, 2);
    np.put32(G4_KEY_MAX_PLAYER, 2);
    np.encode(&stream);
    bufferevent_write(npc->bev, stream.buffer(), stream.size());
    ALOG_INFO("[%s] Match request", npc->desc());
}

static void rematch_timeoutcb(evutil_socket_t fd, short what, void *arg)
{
    NPC *npc = (NPC*)arg;
    match(npc);
}

static void rematch(NPC *npc)
{
    npc->rematch_timer = evtimer_new(base, rematch_timeoutcb, npc);
    evtimer_add(npc->rematch_timer, &rematch_timeout);
}

static void stop_game(NPC *npc)
{
    if (npc->sync_timer != NULL)
    {
        evtimer_del(npc->sync_timer);
    }
    
    G4OutStream stream;
    G4NextPacket np;
    np._packetId = G4_COMM_PLAYER_LEAVE;
    np.encode(&stream);
    bufferevent_write(npc->bev, stream.buffer(), stream.size());
    ALOG_INFO("[%s] Leave game", npc->desc());
    rematch(npc);
}

static bool is_timeover(NPC *npc)
{
    return time(NULL) - npc->start_time > npc->game_time;
}
static void sync_timeoutcb(evutil_socket_t fd, short what, void *arg)
{
    NPC *npc = (NPC *)arg;
//    PAIR_MESSAGE_ID_GAMEDATA
//    PAIR_KEY_SCORE int 分数
//    PAIR_KEY_REMAINTIME int 剩下时间 从300秒开始
//    PAIR_KEY_HINT int 这个是提示道具的个数 5开始
//    PAIR_KEY_BOMB int 这是自己的炸弹个数 从3开始
//    PAIR_KEY_DEMON int 这是自己现在显示的demon icon个数
    
    G4NextPacket np;
    G4OutStream stream;
    np._serverIndicator = 3;
    bool timeover = is_timeover(npc);
    if (timeover)
    {
        np._packetId = PAIR_MESSAGE_ID_TIMEOUT;
    }
    else
    {
        np._packetId = PAIR_MESSAGE_ID_GAMEDATA;
        if ( (time(NULL) - npc->pause_time) > PAUSE_TIME)
        {
            //
            npc->score += rand() % 100;
            int rand_tmp = rand() % 100;
            if (rand_tmp > 50)
                npc->hint++;
            else if (npc->hint > 0)
                npc->hint--;
            
            rand_tmp = rand() % 100;
            if (rand_tmp > 50)
                npc->bomb++;
            else if(npc->bomb > 0)
                npc->bomb--;
        }
        np.encode(&stream);
        np.put32(PAIR_KEY_SCORE, npc->score);
        np.put32(PAIR_KEY_REMAINTIME, npc->game_time);
        np.put32(PAIR_KEY_HINT, npc->hint);
        np.put32(PAIR_KEY_BOMB, npc->bomb);
        np.put32(PAIR_KEY_DEMON, npc->demon);
    }
    
    bufferevent_write(npc->bev, stream.buffer(), stream.size());
    if (timeover)
    {
        ALOG_INFO("[%s] Time over", npc->desc());
        stop_game(npc);
    }
    else {
        npc->sync_timer = evtimer_new(base, sync_timeoutcb, npc);
        evtimer_add(npc->sync_timer, &sync_timeout);  
    }
}
static void adjust_game(NPC *npc, G4NextPacket &np)
{
    // @TODO
}
static void pause_game(NPC *npc)
{
    npc->pause_time = time(NULL);
}
static void on_start_game(NPC *npc)
{
    npc->sync_timer = evtimer_new(base, sync_timeoutcb, npc);
    evtimer_add(npc->sync_timer, &sync_timeout);
    npc->start_time = time(NULL);
    npc->game_time = DEFAULT_GAME_TIME;
    ALOG_INFO("[%s] Game start", npc->desc());
}


static void prepare_game(NPC *npc)
{
    G4NextPacket np;
    np._serverIndicator = 3;
    np._packetId = PAIR_MESSAGE_ID_CELL_ARRAY;
    
    G4OutStream stream;
    np.encode(&stream);
    bufferevent_write(npc->bev, stream.buffer(), stream.size());
    ALOG_INFO("[%s] Send PAIR_MESSAGE_ID_CELL_ARRAY", npc->desc());
}

static void on_prepare_game(NPC *npc)
{
    G4NextPacket np;
    np._serverIndicator = 3;
    np._packetId = PAIR_MESSAGE_ID_CELL_ARRAY_ACK;
    
    G4OutStream stream;
    np.encode(&stream);
    bufferevent_write(npc->bev, stream.buffer(), stream.size());
    ALOG_INFO("[%s] Send PAIR_MESSAGE_ID_CELL_ARRAY_ACK", npc->desc());

    on_start_game(npc);
}

static void send_id_serial(NPC *npc)
{
    // just forward a hello
    G4OutStream stream;
    G4NextPacket np;
    np._serverIndicator = 3;
    np._packetId = PAIR_MESSAGE_ID_SERIAL;
    npc->id_serial = 0;
    if (!asNPC)
        npc->id_serial = 1+ (rand()%65535);
    
    np.put32(PAIR_KEY_SERIAL, npc->id_serial);
    np.put32(PAIR_KEY_CAPABILITY, 1);
    
    np.encode(&stream);
    bufferevent_write(npc->bev, stream.buffer(), stream.size());
    ALOG_INFO("[%s] Send PAIR_MESSAGE_ID_SERIAL", npc->desc());
}

static void on_id_serail(NPC *npc, G4NextPacket &np)
{
    unsigned int peer_id;
    np.get32(PAIR_KEY_SERIAL, peer_id);
    if (npc->id_serial > peer_id)
        prepare_game(npc);
    else {
        ALOG_INFO("[%s] Waiting peer to start game", npc->desc());
    }
    // else wait peer to start the game.
}

static void handle_game_command(NPC *npc, G4NextPacket &np)
{
    switch (np._packetId) {
        case PAIR_MESSAGE_ID_SERIAL:
            ALOG_INFO("[%s] Got PAIR_MESSAGE_ID_SERIAL", npc->desc());
            on_id_serail(npc, np);
            break;
        case PAIR_MESSAGE_ID_CELL_ARRAY:
            ALOG_INFO("[%s] Got PAIR_MESSAGE_ID_CELL_ARRAY", npc->desc());
            on_prepare_game(npc);
            break;
        case PAIR_MESSAGE_ID_CELL_ARRAY_ACK:
            ALOG_INFO("[%s] Got PAIR_MESSAGE_ID_CELL_ARRAY_ACK", npc->desc());
            on_start_game(npc);
            break;
        case PAIR_MESSAGE_ID_TIMEOUT:
            ALOG_INFO("[%s] Got PAIR_MESSAGE_ID_TIMEOUT", npc->desc());
            break;
        case PAIR_MESSAGE_ID_GAMEDATA:
            ALOG_INFO("[%s] Got PAIR_MESSAGE_ID_GAMEDATA", npc->desc());
            adjust_game(npc, np);
            break;
        case PAIR_MESSAGE_ID_RESIGN:
            ALOG_INFO("[%s] Got PAIR_MESSAGE_ID_RESIGN", npc->desc());
            stop_game(npc);
            break;
        case PAIR_MESSAGE_ID_DEMON:
        case PAIR_MESSAGE_ID_COMBO:
        case PAIR_MESSAGE_ID_COIN:
            ALOG_INFO("[%s] Got %d", npc->desc(), np._packetId);
            break;
        case PAIR_MESSAGE_ID_CROW:
            ALOG_INFO("[%s] Got PAIR_MESSAGE_ID_CROW", npc->desc());
            pause_game(npc);
            break;
        default:
            ALOG_INFO("[%s] Unkown command:%d", npc->desc(), (int)np._packetId);
            break;
    }
}

static void handle_command(NPC *npc, G4NextPacket &np)
{
    switch (np._packetId) {
        case G4_COMM_PLAYER_LOGIN:
        case G4_COMM_NPC_LOGIN:
        {
            ALOG_INFO("[%s] Login(%d) as %s", 
                   npc->desc(), np._result, asNPC ? "NPC" : "player");
            
            if (np._result == 0)
                match(npc);
        }
            break;
        case G4_COMM_MATCH_REQUEST:
        {
            ALOG_INFO("[%s] Match %s", npc->desc(), np._result ? "FAILED":"OK");
            if (np._result != 0)
                rematch(npc);
        }
            break;
        case G4_COMM_PLAYER_MATCHED:
        {
            ALOG_INFO("[%s] Matched(%d)", npc->desc(), np._result);
            std::vector<std::string> playerid;
            np.getss(G4_KEY_PLAYER_ID, playerid);
            for (auto it = playerid.begin(); it != playerid.end(); ++it)
            {
                ALOG_INFO("[%s] Matched player:%s", npc->desc(), it->c_str());
            }
        }
            break;
            
        case G4_COMM_MATCH_CREATED:
        {
            ALOG_INFO("[%s] Game start(%d)", npc->desc(), np._result);
            std::vector<std::string> playerid;
            np.getss(G4_KEY_PLAYER_ID, playerid);
            for (auto it = playerid.begin(); it != playerid.end(); ++it)
            {
                ALOG_INFO("[%s] Player:%s", npc->desc(), it->c_str());
            }  
            send_id_serial(npc);
        }
            break;
            
        case G4_COMM_MATCH_DISMISSED:
        {
            ALOG_INFO("[%s] Game stop(%d)", npc->desc(), np._result);
            std::vector<std::string> playerid;
            np.getss(G4_KEY_PLAYER_ID, playerid);
            for (auto it = playerid.begin(); it != playerid.end(); ++it)
            {
                ALOG_INFO("[%s] Player:%s", npc->desc(), it->c_str());
            }
            stop_game(npc);
        }
            break;
        default:
            handle_game_command(npc, np);
            break;
    }
}

static void readcb(struct bufferevent *bev, void *ctx)
{
    NPC *npc = (NPC*)ctx;
    while(true){
        // loop until all command processed
        struct evbuffer *input = bufferevent_get_input(bev);
        size_t buffLength = evbuffer_get_length(input);
        if (buffLength < 2)
        {
            return ;
        }
        
        unsigned char lenbuf[2];
        evbuffer_copyout(input, lenbuf, 2);
        size_t length = (lenbuf[0] | lenbuf[1] << 8);
        if (buffLength-2 < length)
        {
            ALOG_INFO("[%s] Waiting command", npc->desc());
            return ;
        }
        // grain the length field
        evbuffer_drain(input, 2);
        EvBufferStream evstream(input);
        G4InStream stream(evstream);
        G4NextPacket np;
        if (!np.decode(&stream))
        {
            ALOG_INFO("[%s] Decode command error", npc->desc());
            return ;
        }
        handle_command(npc, np);
    }
}

static void login(NPC *npc){
    G4NextPacket np;
    if (asNPC)
        np._packetId = G4_COMM_NPC_LOGIN;
    else 
        np._packetId = G4_COMM_PLAYER_LOGIN;
    
    np.puts(G4_KEY_PLAYER_ID, npc->playerid.c_str());
    np.puts(G4_KEY_PASSWORD, npc->passwd.c_str());
    G4OutStream stream;
    np.encode(&stream);
    bufferevent_write(npc->bev, stream.buffer(), stream.size());
    ALOG_INFO("[%s] Login request", npc->desc());
}

static void eventcb(struct bufferevent *bev, short events, void *ctx)
{
    NPC *npc = (NPC *)ctx;
	if (events & BEV_EVENT_CONNECTED) {
		evutil_socket_t fd = bufferevent_getfd(bev);
		set_tcp_no_delay(fd);
		ALOG_INFO("[%s] Connected", npc->desc());
        login(npc);
	} else if (events & BEV_EVENT_ERROR) {
		ALOG_INFO("[%s] NOT Connected", npc->desc());
	}
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		ALOG_INFO("Usage: client <host> <port> [sessions]");
		return 1;
	}
    int sessions = 1;
    if (argc > 3)
        sessions = atoi(argv[3]);
    if (sessions == 0)
    {
        asNPC = false;
        sessions = 1;
    }
    const char *host = argv[1];
	int port = atoi(argv[2]);
    
    // read name data
    std::ifstream infile("name.txt");
    if (!infile) {
        ALOG_INFO("Can't open name.txt");
        return 1;
    }
    
    std::istream_iterator<std::string> is(infile);
    std::istream_iterator<std::string> eof;
    copy(is, eof, back_inserter(names));

    // initialiaze timevals
    rematch_timeout.tv_sec = 5;
    rematch_timeout.tv_usec = 0;
    reconnect_timeout.tv_usec = 10;
    reconnect_timeout.tv_usec = 0;
    sync_timeout.tv_sec = 0;
    sync_timeout.tv_usec = 500*1000;
    
    // create event base
	base = event_base_new();
	if (!base) {
		ALOG_INFO("Couldn't open event base");
		return 1;
	}
    std::vector<NPC> npc(sessions);
    std::vector<bufferevent*> bev(sessions);
    srand(time(NULL));
    for (int i=0; i<sessions; i++)
    {
        bev[i] = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
        npc[i].bev = bev[i];
        size_t nameIndex = rand()%(names.size()-1);
        npc[i].playerid = names[nameIndex];
        
        bufferevent_setcb(bev[i], readcb, NULL, eventcb, &npc[i]);
        bufferevent_enable(bev[i], EV_READ|EV_WRITE);
        ALOG_INFO("[%s] Connecting to %s:%d", npc[i].desc(), host, port);
        if(bufferevent_socket_connect_hostname(bev[i], NULL, AF_INET, host, port) != 0)
        {
            ALOG_INFO("[%s] Connect host failed", npc[i].desc());
            return 1;
        }
    }
    
	event_base_dispatch(base);
    for (int i=0; i<sessions; ++i)
        bufferevent_free(bev[i]);
	event_base_free(base);
	return 0;
}

