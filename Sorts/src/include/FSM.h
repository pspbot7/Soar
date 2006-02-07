#include<map>
#include<string>
#include<list>
#include<vector>


#include "GameStateModule.H"
#include "GfxModule.H"
#include "Game.H"
#include "GameChanges.H"
#include "GameTile.H"
#include "ServerObjData.H"
#include "Options.H"

class FSM{
 public:
	FSM();
	virtual ~FSM();

	virtual bool update()=0;

	virtual void setGameObject(GameObj *g){gob = g;}
	virtual GameObj *getGameObject(){return gob;}
	virtual void setParams(std::vector<sint4>);

 //private:
	std::string name;
	GameObj *gob;
	Vector<sint4> params;
};
