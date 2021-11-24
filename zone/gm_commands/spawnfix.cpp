#include "../client.h"

void command_spawnfix(Client *c, const Seperator *sep)
{
	Mob* target_mob = c->GetTarget();
	if (!target_mob || !target_mob->IsNPC()) {
		c->Message(Chat::White, "Error: #spawnfix: Need an NPC target.");
		return;
	}

	Spawn2* s2 = target_mob->CastToNPC()->respawn2;

	if (!s2) {
		c->Message(Chat::White, "#spawnfix FAILED -- cannot determine which spawn entry in the database this mob came from.");
		c->Message(0, "Creating a new spawn from scratch...");
		std::string getMaxSpawngroupIds = "SELECT id FROM spawngroup ORDER BY id DESC LIMIT 0, 1";
		auto results = database.QueryDatabase(getMaxSpawngroupIds);
		auto lastSpawnGroupId = 0;
		if (results.Success())
		{
			auto row = results.begin();
			lastSpawnGroupId = atoi(row[0]);
		}
		else
		{
			c->Message(13, "Error getting last spawngroup id, exiting command.");
			return;
		}
		c->Message(10, "Inserting new spawngroup...");

		std::string spawngroupquery = StringFormat("insert into spawngroup(name, spawn_limit, dist, max_x, min_x, max_y, min_y, delay, mindelay, despawn, despawn_timer) values('spfix_%s_%i', '0','0','0','0','0','0', '0', '15000','0','100');", zone->GetShortName(), lastSpawnGroupId + 1);

		auto spawngroupResults = database.QueryDatabase(spawngroupquery);
		if (!spawngroupResults.Success())
		{
			c->Message(13, "Error inserting spawngroup, exiting command.");
			return;
		}

		auto newSpawnGroupResult = database.QueryDatabase(getMaxSpawngroupIds);
		if (newSpawnGroupResult.Success())
		{
			auto row = newSpawnGroupResult.begin();
			lastSpawnGroupId = atoi(row[0]);
		}
		else
		{
			c->Message(13, "Error getting last spawngroup id, exiting command.");
			return;
		}

		c->Message(10, "Inserting new spawnentry...");
		std::string spawnentryquery = StringFormat("insert into spawnentry values('%i', '%i', '100', '1');", lastSpawnGroupId, target_mob->CastToNPC()->GetNPCTypeID());
		auto spawnentryresults = database.QueryDatabase(spawnentryquery);
		if (!spawnentryresults.Success())
		{
			c->Message(13, "Error inserting sppawnentry.");
			return;
		}

		c->Message(10, "Inserting spawn2 entry...");

		std::string spawn2entryquery = StringFormat("insert into spawn2(spawngroupID, zone, version,x,y,z,heading,respawntime,variance,pathgrid,_condition, cond_value,enabled,animation) values('%i','%s','0','%f','%f','%f','%f','600','0','0','0','1','1','0');", lastSpawnGroupId, zone->GetShortName(), c->GetX(), c->GetY(), c->GetZ(), c->GetHeading());
		auto spawn2entryResults = database.QueryDatabase(spawn2entryquery);
		if (spawn2entryResults.Success())
		{
			c->Message(0, "Spawngroup added successfully.");
			target_mob->Depop(false);
		}
		else
		{
			c->Message(13, "Error inserting spawn2 entry, exiting command.");
			return;
		}

		return;
	}

	std::string query = StringFormat(
		"UPDATE spawn2 SET x = '%f', y = '%f', z = '%f', heading = '%f' WHERE id = '%i'",
		c->GetX(),
		c->GetY(),
		target_mob->GetFixedZ(c->GetPosition()),
		c->GetHeading(),
		s2->GetID()
	);
	auto results = content_db.QueryDatabase(query);
	if (!results.Success()) {
		c->Message(Chat::Red, "Update failed! MySQL gave the following error:");
		c->Message(Chat::Red, results.ErrorMessage().c_str());
		return;
	}

	c->Message(Chat::White, "Updating coordinates successful.");
	target_mob->Depop(false);
}

