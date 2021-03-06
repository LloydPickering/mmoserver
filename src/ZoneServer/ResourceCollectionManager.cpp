 /*
---------------------------------------------------------------------------------------
This source file is part of SWG:ANH (Star Wars Galaxies - A New Hope - Server Emulator)

For more information, visit http://www.swganh.com

Copyright (c) 2006 - 2010 The SWG:ANH Team
---------------------------------------------------------------------------------------
Use of this source code is governed by the GPL v3 license that can be found
in the COPYING file or at http://www.gnu.org/licenses/gpl-3.0.html

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
---------------------------------------------------------------------------------------
*/

#include "PlayerObject.h"
#include "ResourceCollectionManager.h"
#include "ResourceCollectionCommand.h"
#include "UIManager.h"
#include "WaypointObject.h"
#include "Inventory.h"
#include "Datapad.h"
#include "WorldManager.h"
#include "MessageLib/MessageLib.h"
#include "LogManager/LogManager.h"
#include "DatabaseManager/Database.h"
#include "DatabaseManager/DatabaseResult.h"
#include "DatabaseManager/DataBinding.h"
#include "Utils/clock.h"
#include "Utils/rand.h"

//======================================================================================================================

bool ResourceCollectionManager::mInsFlag = false;
ResourceCollectionManager* ResourceCollectionManager::mSingleton = NULL;

//======================================================================================================================

ResourceCollectionManager::ResourceCollectionManager(Database* database) :
mDatabase(database),
mDBAsyncPool(sizeof(RCMAsyncContainer))
{
	_setupDatabindings();

	// load sample costs
	mDatabase->ExecuteSqlAsync(this,new(mDBAsyncPool.ordered_malloc()) RCMAsyncContainer(RCMQuery_SampleCosts),
		"select id, commandname, healthcost, actioncost, mindcost, damage_multiplier from command_table where commandname in ('dosample');");
	
	mDatabase->ExecuteSqlAsync(this,new(mDBAsyncPool.ordered_malloc()) RCMAsyncContainer(RCMQuery_SurveyCosts),
		"select id, commandname, healthcost, actioncost, mindcost, damage_multiplier from command_table where commandname in ('requestSurvey');");

}

//======================================================================================================================

ResourceCollectionManager* ResourceCollectionManager::Init(Database* database)
{
	if(mInsFlag == false)
	{
		mSingleton = new ResourceCollectionManager(database);
		mInsFlag = true;
		return mSingleton;
	}
	else
		return mSingleton;
}

//======================================================================================================================

ResourceCollectionManager::~ResourceCollectionManager()
{
	_destroyDatabindings();

	mInsFlag = false;
	delete(mSingleton);
}

void ResourceCollectionManager::_setupDatabindings()
{
	mCommandCostBinding = mDatabase->CreateDataBinding(6);
	mCommandCostBinding->addField(DFT_uint32,offsetof(ResourceCollectionCommand,mId),4,0);
	mCommandCostBinding->addField(DFT_bstring,offsetof(ResourceCollectionCommand,mCommandName),255,1);
	mCommandCostBinding->addField(DFT_int32,offsetof(ResourceCollectionCommand,mHealthCost),4,2);
	mCommandCostBinding->addField(DFT_int32,offsetof(ResourceCollectionCommand,mActionCost),4,3);
	mCommandCostBinding->addField(DFT_int32,offsetof(ResourceCollectionCommand,mMindCost),4,4);
	mCommandCostBinding->addField(DFT_int32,offsetof(ResourceCollectionCommand,mDamageModifier),4,5);
}

//======================================================================================================================

void ResourceCollectionManager::_destroyDatabindings()
{
	mDatabase->DestroyDataBinding(mCommandCostBinding);
}

//======================================================================================================================
void ResourceCollectionManager::handleDatabaseJobComplete(void* ref,DatabaseResult* result)
{
	RCMAsyncContainer* asyncContainer = reinterpret_cast<RCMAsyncContainer*>(ref);
	
	switch (asyncContainer->mQueryType)
	{
		case RCMQuery_SampleCosts:
			{
				ResourceCollectionCommand* cCommand = new ResourceCollectionCommand();

				if (result->getRowCount())
				{
					result->GetNextRow(mCommandCostBinding, cCommand);
					this->sampleActionCost = cCommand->getActionCost();
					this->sampleHealthCost = cCommand->getHealthCost();
					this->sampleMindCost = cCommand->getMindCost();
					this->sampleRadioactiveDamageModifier = cCommand->getDamageModifier();
					
				}

				if(result->getRowCount())
					gLogger->log(LogManager::NOTICE,"Loaded sample costs.");

			}
			break;

		case RCMQuery_SurveyCosts:
			{
				ResourceCollectionCommand* cCommand = new ResourceCollectionCommand();
				if (result->getRowCount())
				{
				
					result->GetNextRow(mCommandCostBinding, cCommand);

					
					this->surveyActionCost = cCommand->getActionCost();
					this->surveyHealthCost = cCommand->getHealthCost();
					this->surveyMindCost = cCommand->getMindCost();
					
				}
				
				if(result->getRowCount())
					gLogger->log(LogManager::NOTICE,"Loaded survey costs.");						

			}
			break;

		default:break;
	}

	mDBAsyncPool.ordered_free(asyncContainer);
}

//=============================================================================
//
// handles any UIWindow callbacks for sampling events
//

void ResourceCollectionManager::handleUIEvent(uint32 action,int32 element,BString inputStr,UIWindow* window)
{
	PlayerObject* player = window->getOwner();
	if(!player)
	{
		return;
	}

	Inventory* inventory = dynamic_cast<Inventory*>(player->getEquipManager()->getEquippedObject(CreatureEquipSlot_Inventory));			
	if(!inventory)
	{
		return;
	}
	
	WindowAsyncContainerCommand* asyncContainer = (WindowAsyncContainerCommand*)window->getAsyncContainer();
	if(!asyncContainer)
		return;

	Ham* ham = player->getHam();
	
	switch(window->getWindowType())
	{
		// Sampling Radioactive Msg Box
		case SUI_Window_SmplRadioactive_MsgBox:
		{
			//we stopped the sampling
			if(action == 1)
			{
				player->getSampleData()->mPassRadioactive = false;
				player->getSampleData()->mPendingSample = false;
				player->setPosture(CreaturePosture_Upright);
				player->updateMovementProperties();
				gMessageLib->sendUpdateMovementProperties(player);
				gMessageLib->sendPostureAndStateUpdate(player);
				gMessageLib->sendSelfPostureUpdate(player);
				return;
			}
			else
			{
				gLogger->log(LogManager::DEBUG,"sampling radioactive box: Yes");
	
				player->getSampleData()->mPassRadioactive = true;
				player->getSampleData()->mPendingSample = true;
				
				if(ham->checkMainPools(0,sampleActionCost,0))
				{

					SurveyTool*			tool					= dynamic_cast<SurveyTool*>(inventory->getObjectById(asyncContainer->ToolId));
					CurrentResource*	resource				= (CurrentResource*)asyncContainer->CurrentResource;
					player->getSampleData()->mNextSampleTime	= Anh_Utils::Clock::getSingleton()->getLocalTime() + 4000;
					player->getController()->addEvent(new SampleEvent(player,tool,resource),4000);
				}
			}
		}
		break;

		case SUI_Window_SmplGamble_ListBox:
		{
			//action == 1 is cancel
			if(action == 1)
			{
				player->getSampleData()->mPendingSample = false;
				player->getSampleData()->mSampleGambleFlag = false;
				player->setPosture(CreaturePosture_Upright);
				player->updateMovementProperties();
				gMessageLib->sendUpdateMovementProperties(player);
				gMessageLib->sendPostureAndStateUpdate(player);
				gMessageLib->sendSelfPostureUpdate(player);
				return;

			}
			else
			{
				if(element == 0)
				{
					player->getSampleData()->mPendingSample = true;
					player->getSampleData()->mSampleGambleFlag = false;

					SurveyTool*			tool		= dynamic_cast<SurveyTool*>(inventory->getObjectById(asyncContainer->ToolId));
					CurrentResource*	resource	= (CurrentResource*)asyncContainer->CurrentResource;
					player->getSampleData()->mNextSampleTime = Anh_Utils::Clock::getSingleton()->getLocalTime() + 1000;
					player->getController()->addEvent(new SampleEvent(player,tool,resource),1000);
					
				}
				else
				{
					gLogger->log(LogManager::DEBUG,"sampling gamble box ... gamble");
					//action costs
					if(!ham->checkMainPools(0,sampleActionCost*2,0))
					{
						player->setPosture(CreaturePosture_Upright);
						player->updateMovementProperties();
						gMessageLib->sendUpdateMovementProperties(player);
						gMessageLib->sendPostureAndStateUpdate(player);
						gMessageLib->sendSelfPostureUpdate(player);
						player->getSampleData()->mSampleEventFlag = false;
						player->getSampleData()->mSampleGambleFlag = false;
                        gMessageLib->SendSystemMessage(::common::OutOfBand("survey", "gamble_no_action"), player);
						return;
					}
					player->getSampleData()->mPendingSample = true;

					//determine whether gamble is good or not
					int32 gambleRoll = int(gRandom->getRand()%2) + 1;

					if(gambleRoll == 1)
					{
						player->getSampleData()->mSampleEventFlag = true;
						player->getSampleData()->mSampleGambleFlag = true;
					}
					else
					{
						player->getSampleData()->mSampleEventFlag = false;
						player->getSampleData()->mSampleGambleFlag = false;
                        gMessageLib->SendSystemMessage(::common::OutOfBand("survey", "gamble_fail"), player);
					}
		
					SurveyTool*			tool		= dynamic_cast<SurveyTool*>(inventory->getObjectById(asyncContainer->ToolId));
					CurrentResource*	resource	= (CurrentResource*)asyncContainer->CurrentResource;
					player->getSampleData()->mNextSampleTime = Anh_Utils::Clock::getSingleton()->getLocalTime() + 1000;
					player->getController()->addEvent(new SampleEvent(player,tool,resource),1000);
					
				}
			}
		}
		break;

		case SUI_Window_SmplWaypNode_ListBox:
		{
			if(action == 0)
			{
				//we hit ok and went for the wp
				if(element == 1)
				{				
					player->getSampleData()->mPendingSample	= false;
					player->getSampleData()->mSampleNodeFlag = true;
					
					player->getSampleData()->Position.x = player->mPosition.x +(((gRandom->getRand()%50)+1));
					player->getSampleData()->Position.z = player->mPosition.z +(((gRandom->getRand()%50)+1));
					player->getSampleData()->zone		= gWorldManager->getZoneId();
					player->getSampleData()->resource	= (CurrentResource*)asyncContainer->CurrentResource;

					
					Datapad* datapad			= player->getDataPad();
                    datapad->requestNewWaypoint("Resource Node", player->getSampleData()->Position ,static_cast<uint16>(gWorldManager->getZoneId()),Waypoint_blue);
                    gMessageLib->SendSystemMessage(::common::OutOfBand("survey", "node_waypoint"), player);

					player->setPosture(CreaturePosture_Upright);
					player->updateMovementProperties();
					gMessageLib->sendUpdateMovementProperties(player);
					gMessageLib->sendPostureAndStateUpdate(player);
					gMessageLib->sendSelfPostureUpdate(player);

					
				return;
				}
				//we ignored the node - so continue sampling
				if(element == 0)
				{				
					player->getSampleData()->mPendingSample = true;
					player->getSampleData()->mSampleGambleFlag = false;

					SurveyTool*			tool		= dynamic_cast<SurveyTool*>(inventory->getObjectById(asyncContainer->ToolId));
					CurrentResource*	resource	= (CurrentResource*)asyncContainer->CurrentResource;
					player->getSampleData()->mNextSampleTime = Anh_Utils::Clock::getSingleton()->getLocalTime() + 10000;
					player->getController()->addEvent(new SampleEvent(player,tool,resource),10000);
					
				}
			}
			else
			{
				gLogger->log(LogManager::DEBUG,"sampling wayp node box action != 1 (stay here?)");
				player->getSampleData()->mPendingSample = false;
				player->getSampleData()->mSampleNodeFlag = false;
				player->getSampleData()->Position.x = 0;
				player->getSampleData()->Position.z = 0;
				player->getSampleData()->resource	= NULL;
				player->getSampleData()->zone		= 0;

				player->setPosture(CreaturePosture_Upright);
				player->updateMovementProperties();
				gMessageLib->sendUpdateMovementProperties(player);
				gMessageLib->sendPostureAndStateUpdate(player);
				gMessageLib->sendSelfPostureUpdate(player);
				
				//continue sampling normally or stop ???
				//SurveyTool*			tool		= dynamic_cast<SurveyTool*>(inventory->getObjectById(asyncContainer->ToolId));
				//CurrentResource*	resource	= (CurrentResource*)asyncContainer->CurrentResource;
				//player->getSampleData()->mNextSampleTime = Anh_Utils::Clock::getSingleton()->getLocalTime() + 30000;
				//player->getController()->addEvent(new SampleEvent(tool,resource),10000);
				return;
				
			}
		}
		break;
	}
	SAFE_DELETE(asyncContainer);
}
