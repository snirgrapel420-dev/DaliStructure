#include "dali/Serialization.h"

namespace dali {
using json::Value;

static Value sectionToJson(const Section& s) {
    Value v = Value::object();
    v["id"] = s.id;
    v["type"] = toString(s.type);
    v["label"] = s.label;
    v["startBar"] = s.startBar;
    v["endBar"] = s.endBar;
    v["durationBars"] = s.durationBars();
    v["startBeat"] = s.startBeat();
    v["endBeat"] = s.endBeat();
    v["startTime"] = s.startTime;
    v["endTime"] = s.endTime;
    v["energy"] = s.energy;
    v["confidence"] = s.confidence;
    Value el = Value::array();
    for (auto& e : s.elements) el.push(e);
    v["elements"] = el;
    v["origin"] = toString(s.origin);
    return v;
}

static bool sectionFromJson(const Value& v, Section& s) {
    if (!v.isObject()) return false;
    s.id = v.get("id").asString();
    if (!parse(v.get("type").asString(), s.type)) s.type = SectionType::Section;
    s.label = v.get("label").asString();
    s.startBar = v.get("startBar").asInt(1);
    s.endBar = v.get("endBar").asInt(1);
    s.startTime = v.get("startTime").asNumber();
    s.endTime = v.get("endTime").asNumber();
    s.energy = v.get("energy").asNumber();
    s.confidence = v.get("confidence").asNumber();
    s.elements.clear();
    for (auto& e : v.get("elements").items()) s.elements.push_back(e.asString());
    parse(v.get("origin").asString(), s.origin);
    return s.endBar >= s.startBar;
}

static Value eventToJson(const Event& e) {
    Value v = Value::object();
    v["id"] = e.id;
    v["type"] = toString(e.type);
    v["category"] = toString(categoryOf(e.type));
    v["label"] = e.label;
    v["bar"] = e.bar;
    v["time"] = e.time;
    v["lengthBars"] = e.lengthBars;
    v["importance"] = toString(e.importance);
    v["confidence"] = e.confidence;
    v["origin"] = toString(e.origin);
    return v;
}

static bool eventFromJson(const Value& v, Event& e) {
    if (!v.isObject()) return false;
    e.id = v.get("id").asString();
    if (!parse(v.get("type").asString(), e.type)) return false;
    e.label = v.get("label").asString();
    e.bar = v.get("bar").asNumber(1);
    e.time = v.get("time").asNumber();
    e.lengthBars = v.get("lengthBars").asNumber();
    if (!parse(v.get("importance").asString(), e.importance)) e.importance = Importance::Medium;
    e.confidence = v.get("confidence").asNumber();
    parse(v.get("origin").asString(), e.origin);
    return true;
}

Value toJson(const StructureDocument& d) {
    Value root = Value::object();
    root["schemaVersion"] = d.schemaVersion;
    root["generator"] = "DaliStructure by Dali Audio";

    Value ref = Value::object();
    ref["filePath"] = d.reference.filePath;
    ref["fileName"] = d.reference.fileName;
    ref["contentHash"] = d.reference.contentHash;
    ref["sampleRate"] = d.reference.sampleRate;
    ref["channels"] = d.reference.channels;
    ref["durationSec"] = d.reference.durationSec;
    ref["bpm"] = d.reference.bpm;
    ref["key"] = d.reference.key.name;
    ref["keyConfidence"] = d.reference.key.confidence;
    ref["bars"] = d.reference.barCount;
    ref["firstDownbeatSec"] = d.reference.firstDownbeatSec;
    root["reference"] = ref;

    Value tm = Value::object();
    tm["beatsPerBar"] = d.tempo.beatsPerBar;
    tm["nominalBpm"] = d.tempo.nominalBpm;
    tm["stable"] = d.tempo.stable;
    tm["beatConfidence"] = d.tempo.beatConfidence;
    tm["downbeatConfidence"] = d.tempo.downbeatConfidence;
    Value beats = Value::array();
    for (double t : d.tempo.beatTimes) beats.push(t);
    tm["beatTimes"] = beats;
    Value segs = Value::array();
    for (auto& s : d.tempo.segments) { Value o = Value::object(); o["startBeat"] = s.startBeat; o["bpm"] = s.bpm; segs.push(o); }
    tm["segments"] = segs;
    root["tempoMap"] = tm;

    auto secArr = [](const std::vector<Section>& v) { Value a = Value::array(); for (auto& s : v) a.push(sectionToJson(s)); return a; };
    auto evArr = [](const std::vector<Event>& v) { Value a = Value::array(); for (auto& e : v) a.push(eventToJson(e)); return a; };
    root["sections"] = secArr(d.sections);
    root["events"] = evArr(d.events);
    root["autoSections"] = secArr(d.autoSections);
    root["autoEvents"] = evArr(d.autoEvents);
    root["userEdited"] = d.userEdited;

    Value en = Value::array();
    for (auto& p : d.energy) { Value o = Value::array(); o.push(p.bar); o.push(p.value); en.push(o); }
    root["energy"] = en;   // compact [bar, value] pairs

    Value els = Value::array();
    for (auto& t : d.elements) {
        Value o = Value::object(); o["name"] = t.name;
        Value a = Value::array(); for (float f : t.activity) a.push((double) f);
        o["activity"] = a; els.push(o);
    }
    root["elements"] = els;

    Value dg = Value::object();
    dg["engineVersion"] = d.diagnostics.engineVersion;
    dg["usedStems"] = d.diagnostics.usedStems;
    dg["usedNeuralBeats"] = d.diagnostics.usedNeuralBeats;
    Value w = Value::array(); for (auto& s : d.diagnostics.warnings) w.push(s);
    dg["warnings"] = w;
    root["diagnostics"] = dg;

    Value al = Value::object();
    al["projectBpm"] = d.alignment.projectBpm;
    al["projectStartBar"] = d.alignment.projectStartBar;
    al["projectSections"] = secArr(d.alignment.projectSections);
    root["alignment"] = al;
    return root;
}

bool fromJson(const Value& root, StructureDocument& out, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    if (!root.isObject()) return fail("not an object");
    const int schema = root.get("schemaVersion").asInt(0);
    if (schema < 1) return fail("missing schemaVersion");
    if (schema > kSchemaVersion) return fail("document was written by a newer DaliStructure");
    // (future migrations: if (schema == 1) migrateV1toV2(...))

    StructureDocument d;
    const Value& ref = root.get("reference");
    d.reference.filePath = ref.get("filePath").asString();
    d.reference.fileName = ref.get("fileName").asString();
    d.reference.contentHash = ref.get("contentHash").asString();
    d.reference.sampleRate = ref.get("sampleRate").asNumber();
    d.reference.channels = ref.get("channels").asInt();
    d.reference.durationSec = ref.get("durationSec").asNumber();
    d.reference.bpm = ref.get("bpm").asNumber();
    d.reference.key.name = ref.get("key").asString();
    d.reference.key.confidence = ref.get("keyConfidence").asNumber();
    d.reference.barCount = ref.get("bars").asInt();
    d.reference.firstDownbeatSec = ref.get("firstDownbeatSec").asNumber();

    const Value& tm = root.get("tempoMap");
    d.tempo.beatsPerBar = tm.get("beatsPerBar").asInt(4);
    if (d.tempo.beatsPerBar < 1 || d.tempo.beatsPerBar > 16) return fail("invalid beatsPerBar");
    d.tempo.nominalBpm = tm.get("nominalBpm").asNumber();
    d.tempo.stable = tm.get("stable").asBool(true);
    d.tempo.beatConfidence = tm.get("beatConfidence").asNumber();
    d.tempo.downbeatConfidence = tm.get("downbeatConfidence").asNumber();
    for (auto& b : tm.get("beatTimes").items()) d.tempo.beatTimes.push_back(b.asNumber());
    for (size_t i = 1; i < d.tempo.beatTimes.size(); ++i)
        if (d.tempo.beatTimes[i] <= d.tempo.beatTimes[i - 1]) return fail("beat times not increasing");
    for (auto& s : tm.get("segments").items()) d.tempo.segments.push_back({s.get("startBeat").asInt(), s.get("bpm").asNumber()});

    auto readSecs = [&](const Value& a, std::vector<Section>& v) {
        for (auto& s : a.items()) { Section x; if (!sectionFromJson(s, x)) return false; v.push_back(x); }
        return true;
    };
    auto readEvs = [&](const Value& a, std::vector<Event>& v) {
        for (auto& s : a.items()) { Event x; if (eventFromJson(s, x)) v.push_back(x); }  // unknown event types are skipped
        return true;
    };
    if (!readSecs(root.get("sections"), d.sections)) return fail("invalid section");
    if (!readSecs(root.get("autoSections"), d.autoSections)) return fail("invalid auto section");
    readEvs(root.get("events"), d.events);
    readEvs(root.get("autoEvents"), d.autoEvents);
    d.userEdited = root.get("userEdited").asBool();

    for (auto& p : root.get("energy").items()) d.energy.push_back({p[0].asNumber(), p[1].asNumber()});
    for (auto& t : root.get("elements").items()) {
        ElementTrack et; et.name = t.get("name").asString();
        for (auto& a : t.get("activity").items()) et.activity.push_back((float) a.asNumber());
        d.elements.push_back(std::move(et));
    }
    const Value& dg = root.get("diagnostics");
    d.diagnostics.engineVersion = dg.get("engineVersion").asString();
    d.diagnostics.usedStems = dg.get("usedStems").asBool();
    d.diagnostics.usedNeuralBeats = dg.get("usedNeuralBeats").asBool();
    for (auto& w : dg.get("warnings").items()) d.diagnostics.warnings.push_back(w.asString());

    const Value& al = root.get("alignment");
    d.alignment.projectBpm = al.get("projectBpm").asNumber();
    d.alignment.projectStartBar = al.get("projectStartBar").asInt(1);
    readSecs(al.get("projectSections"), d.alignment.projectSections);

    out = std::move(d);
    return true;
}

std::string serialize(const StructureDocument& doc, bool pretty) { return toJson(doc).dump(pretty); }

bool deserialize(const std::string& text, StructureDocument& doc, std::string* error) {
    Value v;
    if (!Value::parse(text, v, error)) return false;
    return fromJson(v, doc, error);
}

} // namespace dali
