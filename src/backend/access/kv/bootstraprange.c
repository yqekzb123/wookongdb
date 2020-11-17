#include "tdb/timestamp_transaction/http.h"
#include "tdb/bootstraprange.h"
#include "cdb/cdbvars.h"
#include "access/xlog.h"
#include "tdb/storage_processor.h"
#include "tdb/kv_universal.h"

static RangeDesc
initFirstRangeDesc(void)
{
	Replica *replicas = (Replica *)palloc0(sizeof(Replica) * 1);
	RangeID rangeID = 0;
	for (int i = 0; i < 1; i++)
	{
		Replica replica = CreateNewReplica(i, GpIdentity.segindex, UNVALID_REPLICA_ID);
		replicas[i] = replica;
	}
	InitKeyDesc initkeydesc = init_basis_in_keydesc(RAW_KEY);
	initkeydesc.init_type = SYSTEM_NULL_KEY;
	initkeydesc.isend = false;
	TupleKeySlice startKey = build_key(initkeydesc);
	initkeydesc.isend = true;
	TupleKeySlice endKey = build_key(initkeydesc);
	RangeDesc range = CreateNewRangeDesc(rangeID, startKey, endKey, replicas, 1);
	return range;
}

static void
storage_get_rangeid(void)
{
	TupleKey rrlkey = palloc0(sizeof(TupleKeyData));
	rrlkey->rel_id = RANGEID;
	rrlkey->indexOid = RANGEID;
	rrlkey->type = GTS_KEY;
	memset(rrlkey->other_data, 0, 4);
	TupleKeySlice key = {rrlkey, sizeof(TupleKeyData)};

	uint64_t t = GetTimeStamp();
	KVEngineTransactionInterface *txn = engine->create_txn(engine, 1, true, t);
	int a, b;
	TupleValueSlice value = txn->get(txn, key, 0, &a, &b);
	t = GetTimeStamp();
	txn->commit_and_destroy(txn, 1, t);

	if (value.len == 0)
	{
		Rangeidvalue = 0;
		return;
	}
	int *rrlv = (int*)value.data->memtuple.PRIVATE_mt_bits;
	if (*rrlv == value.data->sysattrs.cid)
		Rangeidvalue = value.data->sysattrs.cid;
	pfree(rrlkey);
}

static void
storage_inc_rangeid(void)
{
	TupleKey rangeidkey = palloc0(sizeof(TupleKeyData));
	rangeidkey->rel_id = RANGEID;
	rangeidkey->indexOid = RANGEID;
	rangeidkey->type = GTS_KEY;
	memset(rangeidkey->other_data, 0, 4);
	TupleKeySlice key = {rangeidkey, sizeof(TupleKeyData)};

	RangeID *rangeidv = palloc0(sizeof(RangeID));
	*rangeidv = Rangeidvalue + 1;

	TupleValue rangeidvalue = palloc0(sizeof(TupleValueData) + sizeof(RangeID));
	rangeidvalue->sysattrs.natts = 0;
	rangeidvalue->sysattrs.xmax = InvalidTransactionId;
	rangeidvalue->sysattrs.infomask = 0;
	rangeidvalue->sysattrs.infomask2 = 0;
	rangeidvalue->sysattrs.cid = Rangeidvalue + 1;
	rangeidvalue->memtuple.PRIVATE_mt_len = sizeof(int);

	memcpy(rangeidvalue->memtuple.PRIVATE_mt_bits, (void *)rangeidv, sizeof(RangeID));

	TupleValueSlice value = {rangeidvalue, sizeof(TupleValueData) + sizeof(RangeID)};

	uint64_t t = GetTimeStamp();
	KVEngineTransactionInterface *txn = engine->create_txn(engine, 1, true, t);
	txn->put(txn, key, value, ROCKS_DEFAULT_CF_I);
	t = GetTimeStamp();
	txn->commit_and_destroy(txn, 1, t);

	pfree(rangeidkey);
	pfree(rangeidv);
	pfree(rangeidvalue);

	Rangeidvalue++;
}

static void
storage_store_rangedesc(RangeDesc range)
{
	Size length = 0;
	char *rangebuffer = TransferRangeDescToBuffer(range, &length);
	TupleValueSlice rangevalue = {(TupleValue)rangebuffer, length};

	TupleKeySlice rangekey = makeRangeDescKey(range.rangeID);

	uint64_t t = GetTimeStamp();
	KVEngineTransactionInterface *txn = engine->create_txn(engine, 1, true, t);
	txn->put(txn, rangekey, rangevalue, ROCKS_DEFAULT_CF_I);
	t = GetTimeStamp();
	txn->commit_and_destroy(txn, 1, t);

	pfree(rangevalue.data);
	pfree(rangekey.data);
}

static void
storage_store_route(RangeDesc range)
{
	Size length = 0;
	char *rangebuffer = TransferRangeDescToBuffer(range, &length);
	TupleValueSlice rangevalue = {(TupleValue)rangebuffer, length};
	bool getkey;
	TupleKeySlice rangekey = makeRouteKey(range.endkey, &getkey);
	if (getkey)
	{
		uint64_t t = GetTimeStamp();
		KVEngineTransactionInterface *txn = engine->create_txn(engine, 1, true, t);
		txn->put(txn, rangekey, rangevalue, ROCKS_DEFAULT_CF_I);
		t = GetTimeStamp();
		txn->commit_and_destroy(txn, 1, t);
	}
	pfree(rangevalue.data);
	pfree(rangekey.data);
}

void
BootstrapInitRange(void)
{
	if (GpIdentity.segindex == -1)
	{
		Gp_role = GP_ROLE_DISPATCH;
		storage_get_rangeid();
		if (Rangeidvalue == 0)
			storage_inc_rangeid();
	}
	else if(!IsRoleMirror())
	{
		Gp_role = GP_ROLE_EXECUTE;
		storage_get_rangeid();
		if (Rangeidvalue == 0)
		{
			RangeDesc first = initFirstRangeDesc();
			rootRouteRangeID = Rangeidvalue;
			storage_store_rangedesc(first);
			storage_store_route(first);
			storage_inc_rangeid();
		}
	}
	else
	{
		//ereport(LOG,(errmsg("bootstap I am mirror.")));
	}
}

void
BootstrapInitStatistics(void)
{
	if (GpIdentity.segindex == -1)
	{
		//Init_M_S_Stat();
	}
	else if(!IsRoleMirror())
	{
		Init_Seg_Stat();
	}
	else
	{
		//ereport(LOG,(errmsg("bootstap I am mirror.")));
	}
}
