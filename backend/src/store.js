import { MongoClient } from "mongodb";

const clone = (value) => structuredClone(value);

class MemoryStore {
  constructor() {
    this.telemetry = [];
    this.alertRecords = [];
    this.mode = "memory";
  }

  async connect() {}
  async close() {}

  async insertTelemetry(record) {
    this.telemetry.push(clone(record));
    if (this.telemetry.length > 20000) this.telemetry.splice(0, 5000);
  }

  async insertAlert(alert) {
    this.alertRecords.push(clone(alert));
    if (this.alertRecords.length > 2000) this.alertRecords.splice(0, 500);
  }

  async latest(deviceId) {
    const records = deviceId
      ? this.telemetry.filter((item) => item.meta.deviceId === deviceId)
      : this.telemetry;
    return clone(records.at(-1) || null);
  }

  async history({ deviceId, from, to, limit = 500 }) {
    return this.telemetry
      .filter((item) => !deviceId || item.meta.deviceId === deviceId)
      .filter((item) => !from || new Date(item.timestamp) >= from)
      .filter((item) => !to || new Date(item.timestamp) <= to)
      .slice(-limit)
      .map(clone);
  }

  async alerts({ deviceId, limit = 100 }) {
    return this.alertRecords
      .filter((item) => !deviceId || item.deviceId === deviceId)
      .slice(-limit)
      .reverse()
      .map(clone);
  }
}

class MongoStore {
  constructor(url, dbName) {
    this.client = new MongoClient(url, { serverSelectionTimeoutMS: 5000 });
    this.dbName = dbName;
    this.mode = "mongodb";
  }

  async connect() {
    await this.client.connect();
    this.db = this.client.db(this.dbName);
    try {
      await this.db.createCollection("telemetry", {
        timeseries: { timeField: "timestamp", metaField: "meta", granularity: "seconds" },
        expireAfterSeconds: 60 * 60 * 24 * 90
      });
    } catch (error) {
      if (error.code !== 48) throw error;
    }
    this.telemetryCollection = this.db.collection("telemetry");
    this.alertCollection = this.db.collection("alerts");
    await this.alertCollection.createIndex({ deviceId: 1, timestamp: -1 });
  }

  async close() {
    await this.client.close();
  }

  async insertTelemetry(record) {
    await this.telemetryCollection.insertOne({ ...record, timestamp: new Date(record.timestamp) });
  }

  async insertAlert(alert) {
    await this.alertCollection.insertOne({ ...alert, timestamp: new Date(alert.timestamp) });
  }

  async latest(deviceId) {
    const filter = deviceId ? { "meta.deviceId": deviceId } : {};
    return this.telemetryCollection.findOne(filter, { sort: { timestamp: -1 } });
  }

  async history({ deviceId, from, to, limit = 500 }) {
    const filter = deviceId ? { "meta.deviceId": deviceId } : {};
    if (from || to) {
      filter.timestamp = {};
      if (from) filter.timestamp.$gte = from;
      if (to) filter.timestamp.$lte = to;
    }
    return this.telemetryCollection.find(filter).sort({ timestamp: -1 }).limit(limit).toArray()
      .then((items) => items.reverse());
  }

  async alerts({ deviceId, limit = 100 }) {
    const filter = deviceId ? { deviceId } : {};
    return this.alertCollection.find(filter).sort({ timestamp: -1 }).limit(limit).toArray();
  }
}

export async function createStore(config) {
  const mongo = new MongoStore(config.mongoUrl, config.mongoDb);
  try {
    await mongo.connect();
    console.log(`[store] connected to MongoDB database ${config.mongoDb}`);
    return mongo;
  } catch (error) {
    await mongo.close().catch(() => {});
    if (config.requireMongo) throw error;
    console.warn(`[store] MongoDB unavailable (${error.message}); using volatile memory store`);
    const memory = new MemoryStore();
    await memory.connect();
    return memory;
  }
}

export { MemoryStore, MongoStore };

