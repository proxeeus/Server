// Proxeeus
#include "../client.h"

void command_setroambox(Client* c, const Seperator* sep)
{
	Mob* targetMob = c->GetTarget();
	if (!targetMob || !targetMob->IsNPC()) {
		c->Message(0, "Error: #setroambox: Need an NPC target.");
		return;
	}
	int spawngroupId = c->GetTarget()->CastToNPC()->GetSpawnGroupId();
	uint32 minx = atoi(sep->arg[1]);
	uint32 maxx = atoi(sep->arg[2]);
	uint32 miny = atoi(sep->arg[3]);
	uint32 maxy = atoi(sep->arg[4]);
	uint32 dist = atoi(sep->arg[5]);
	uint32 delay = atoi(sep->arg[6]);

	std::string query = StringFormat("UPDATE spawngroup SET min_x = '%i', max_x = '%i', min_y = '%i', max_y='%i', dist='%i', delay='%i' WHERE id = '%i'",
		minx, maxx, miny, maxy, dist, delay, spawngroupId);

	//c->Message(0, "UPDATE spawngroup SET min_x = '%i', max_x = '%i', min_y = '%i', max_y='%i, dist='%i', delay='%i' WHERE id = '%i'",
	//	minx,maxx, miny,maxy,dist,delay, spawngroupId);

	auto results = database.QueryDatabase(query);
	if (!results.Success()) {
		c->Message(13, "Update failed! MySQL gave the following error:");
		c->Message(13, results.ErrorMessage().c_str());
		return;
	}

	c->Message(0, "Updating roambox coordinates successful.");

	//c->Message(0, "COMMAND DEPRECATED.");
}
//
