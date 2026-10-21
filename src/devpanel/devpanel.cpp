#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "httplib.h"
#include "json/json.hpp"

#include "ultramodern/ultramodern.hpp"

#include "q64_devpanel.h"
#include "game_memory.h"

using json = nlohmann::json;

namespace q64::devpanel {

namespace {

// Addresses below are for Quest 64 USA 1.0. See research/spells/README.md in the project notes.
// The spell data block is copied to RAM at boot and stays resident.
constexpr uint32_t spell_def_table = 0x800C1B14;   // SpellDefinition*[5]
constexpr uint32_t spell_anim_table = 0x800C3520;  // SpellAnimDefinition*[4]
constexpr uint32_t spell_name_table = 0x800C045C;  // char*[77]
constexpr uint32_t spell_def_size = 0x44;
constexpr uint32_t spell_anim_size = 0x48;
constexpr int spell_schools = 5;
constexpr int player_schools = 4;
constexpr int spells_per_school = 15;
constexpr int enemy_spell_count = 17;
constexpr int spell_name_count = player_schools * spells_per_school + enemy_spell_count;

// Controller input held by POST /api/input, merged into the real input by injected_input().
struct InjectedInput {
    uint16_t buttons = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::chrono::steady_clock::time_point until{};
};
std::mutex injected_mutex;
InjectedInput injected;

enum class FieldType { U8, U16, S16, U32, F32 };

struct Field {
    const char* key;
    const char* label;
    uint32_t offset;
    FieldType type;
    const char* help;
};

constexpr Field spell_fields[] = {
    { "levelRequirement", "Level required",  0x00, FieldType::U16, "Element level needed to cast" },
    { "usageFlags",       "Usage flags",     0x02, FieldType::U16, "1 = battle, 2 = field, 4 = pierces (hits multiple targets)" },
    { "inputCount",       "Presses",         0x04, FieldType::U16, "Element presses to cast (1-3)" },
    { "input1",           "Press 1",         0x06, FieldType::U8,  "0 Fire, 1 Earth, 2 Water, 3 Wind" },
    { "input2",           "Press 2",         0x07, FieldType::U8,  "0 Fire, 1 Earth, 2 Water, 3 Wind" },
    { "input3",           "Press 3",         0x08, FieldType::U8,  "0 Fire, 1 Earth, 2 Water, 3 Wind" },
    { "range",            "Range",           0x0A, FieldType::U16, "Only used when projectile type is 0" },
    { "baseDamage",       "Base damage",     0x0C, FieldType::U16, "Heal amount for healing spells" },
    { "accuracy",         "Accuracy",        0x0E, FieldType::U16, "Percent" },
    { "spellType",        "Spell type",      0x10, FieldType::U16, "Effect handler, 0x00-0x14" },
    { "projectileType",   "Projectile type", 0x12, FieldType::U16, "0 free, 1 rise/fall only, 2 Magnet Rock" },
    { "travelHeight",     "Travel height",   0x14, FieldType::U16, "" },
    { "autoHit",          "Auto hit",        0x16, FieldType::U16, "1 = always hits" },
    { "target",           "Target",          0x18, FieldType::U16, "0/1 enemy, 2 self" },
    { "unk1A",            "unk 0x1A",        0x1A, FieldType::U16, "" },
    { "size",             "Size",            0x1C, FieldType::F32, "" },
    { "hitCount",         "Shots",           0x20, FieldType::U16, "" },
    { "hitDelay",         "Shot delay",      0x22, FieldType::U16, "Frames/2 between shots" },
    { "startDelay",       "Start delay",     0x24, FieldType::U16, "Frames/2 before it takes effect" },
    { "startPoint",       "Start point",     0x26, FieldType::U16, "0 feet, 1 behind, 2/4 above head, 3 in front, 5 far above, 6 target's feet, 7 random above, 8 side" },
    { "homing",           "Homing",          0x28, FieldType::U16, "0 none, 1 slight, 3 good" },
    { "angle",            "Angle",           0x2A, FieldType::U16, "" },
    { "direction",        "Direction",       0x2C, FieldType::U16, "" },
    { "speedDesired",     "Speed (desired)", 0x2E, FieldType::S16, "" },
    { "speedInitial",     "Speed (initial)", 0x30, FieldType::S16, "" },
    { "compression",      "Compression",     0x32, FieldType::U8,  "" },
    { "vampireTouch",     "Vampire's Touch", 0x33, FieldType::U8,  "Crashes when combined with damage" },
    { "attackUp",         "Attack up",       0x34, FieldType::U8,  "1-2" },
    { "restriction",      "Restriction",     0x35, FieldType::U8,  "" },
    { "movement",         "Movement",        0x36, FieldType::U8,  "1 slow, 2 fast" },
    { "agility",          "Agility",         0x37, FieldType::U8,  "1-2 evade, 3 slow" },
    { "silence",          "Silence",         0x38, FieldType::U8,  "" },
    { "soulSearch",       "Soul Searcher",   0x39, FieldType::U8,  "" },
    { "magicBarrier",     "Magic Barrier",   0x3A, FieldType::U8,  "" },
    { "confusion",        "Confusion",       0x3B, FieldType::U8,  "" },
    { "defense",          "Defense",         0x3C, FieldType::U8,  "1-2 down, 3-4 up" },
    { "unk3D",            "unk 0x3D",        0x3D, FieldType::U8,  "" },
    { "unk3E",            "unk 0x3E",        0x3E, FieldType::U16, "" },
    { "unk40",            "unk 0x40",        0x40, FieldType::U16, "" },
    { "unk42",            "unk 0x42",        0x42, FieldType::U16, "Used by Invalidity" },
};

// One visual stage; 4 per spell, 0x10 bytes apart. Sounds follow at 0x40.
constexpr Field anim_stage_fields[] = {
    { "model",    "Model",    0x00, FieldType::S16, "-1 = stage unused" },
    { "color",    "Color",    0x02, FieldType::U16, "Low byte is the palette" },
    { "duration", "Duration", 0x04, FieldType::U16, "" },
    { "unk6",     "unk 0x06", 0x06, FieldType::U16, "" },
    { "sizeX",    "Size X",   0x08, FieldType::F32, "" },
    { "sizeY",    "Size Y",   0x0C, FieldType::F32, "" },
};
constexpr uint32_t anim_stage_stride = 0x10;
constexpr int anim_stage_count = 4;
constexpr uint32_t anim_sounds_offset = 0x40;

// Brian's stats. Same layout as the new-game stats table at ROM 0x54908. Note: the community RAM
// notes list fields smaller than 4 bytes byte-swapped (as seen in an emulator's word-swapped view).
constexpr Field brian_fields[] = {
    { "hp",         "HP",          0x8007BA84, FieldType::U16, "" },
    { "maxHp",      "Max HP",      0x8007BA86, FieldType::U16, "" },
    { "mp",         "MP",          0x8007BA88, FieldType::U16, "" },
    { "maxMp",      "Max MP",      0x8007BA8A, FieldType::U16, "" },
    { "agility",    "Agility",     0x8007BA8C, FieldType::U16, "" },
    { "defense",    "Defense",     0x8007BA8E, FieldType::U16, "" },
    { "elementExp", "Element EXP", 0x8007BA90, FieldType::U32, "" },
    { "fire",       "Fire level",  0x8007BAA4, FieldType::U8,  "" },
    { "earth",      "Earth level", 0x8007BAA5, FieldType::U8,  "" },
    { "water",      "Water level", 0x8007BAA6, FieldType::U8,  "" },
    { "wind",       "Wind level",  0x8007BAA7, FieldType::U8,  "" },
};

GameMemory memory;
std::unique_ptr<httplib::Server> server;
std::thread server_thread;

const char* type_name(FieldType type) {
    switch (type) {
        case FieldType::U8: return "u8";
        case FieldType::U16: return "u16";
        case FieldType::S16: return "s16";
        case FieldType::U32: return "u32";
        case FieldType::F32: return "f32";
    }
    return "?";
}

json read_field(uint32_t addr, FieldType type) {
    switch (type) {
        case FieldType::U8: return memory.read_u8(addr);
        case FieldType::U16: return memory.read_u16(addr);
        case FieldType::S16: return (int16_t)memory.read_u16(addr);
        case FieldType::U32: return memory.read_u32(addr);
        case FieldType::F32: return memory.read_f32(addr);
    }
    return nullptr;
}

void write_field(uint32_t addr, FieldType type, const json& value) {
    switch (type) {
        case FieldType::U8: memory.write_u8(addr, (uint8_t)value.get<int64_t>()); break;
        case FieldType::U16: memory.write_u16(addr, (uint16_t)value.get<int64_t>()); break;
        case FieldType::S16: memory.write_u16(addr, (uint16_t)(int16_t)value.get<int64_t>()); break;
        case FieldType::U32: memory.write_u32(addr, (uint32_t)value.get<int64_t>()); break;
        case FieldType::F32: memory.write_f32(addr, value.get<float>()); break;
    }
}

template <size_t N>
json fields_schema(const Field (&fields)[N]) {
    json out = json::array();
    for (const Field& f : fields) {
        out.push_back({ {"key", f.key}, {"label", f.label}, {"offset", f.offset}, {"type", type_name(f.type)}, {"help", f.help} });
    }
    return out;
}

template <size_t N>
json read_fields(uint32_t base, const Field (&fields)[N]) {
    json out = json::object();
    for (const Field& f : fields) {
        out[f.key] = read_field(base + f.offset, f.type);
    }
    return out;
}

// Writes the keys present in `values`. Returns false (writing nothing) if any key is unknown.
template <size_t N>
bool write_fields(uint32_t base, const Field (&fields)[N], const json& values, std::string& error) {
    for (auto it = values.begin(); it != values.end(); ++it) {
        bool found = false;
        for (const Field& f : fields) {
            found |= (it.key() == f.key);
        }
        if (!found || !it.value().is_number()) {
            error = "bad field: " + it.key();
            return false;
        }
    }
    for (const Field& f : fields) {
        if (values.contains(f.key)) {
            write_field(base + f.offset, f.type, values[f.key]);
        }
    }
    return true;
}

std::string path_string(const std::filesystem::path& path) {
    std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

bool spell_tables_loaded() {
    // The pointer table holds 0x800Cxxxx pointers once the boot data load has run.
    return (memory.read_u32(spell_def_table) & 0xFFFF0000) == 0x800C0000;
}

uint32_t spell_def_addr(int school, int index) {
    return memory.read_u32(spell_def_table + school * 4) + index * spell_def_size;
}

uint32_t spell_anim_addr(int school, int index) {
    return memory.read_u32(spell_anim_table + school * 4) + index * spell_anim_size;
}

int spell_count(int school) {
    return school < player_schools ? spells_per_school : enemy_spell_count;
}

int name_index(int school, int index) {
    return school * spells_per_school + index;
}

// Bytes available for a name: distance to the next name in memory, so edits never spill over.
uint32_t name_slot_size(uint32_t name_addr) {
    uint32_t next = 0xFFFFFFFF;
    for (int i = 0; i < spell_name_count; i++) {
        uint32_t other = memory.read_u32(spell_name_table + i * 4);
        if (other > name_addr && other < next) {
            next = other;
        }
    }
    return next == 0xFFFFFFFF ? 16 : std::min<uint32_t>(next - name_addr, 32);
}

std::string read_name(uint32_t addr, uint32_t max_len) {
    std::string out;
    for (uint32_t i = 0; i < max_len; i++) {
        char c = (char)memory.read_u8(addr + i);
        if (c == '\0') {
            break;
        }
        out += c;
    }
    return out;
}

json spell_json(int school, int index) {
    uint32_t def = spell_def_addr(school, index);
    uint32_t name_addr = memory.read_u32(spell_name_table + name_index(school, index) * 4);
    json out = {
        {"school", school}, {"index", index}, {"id", (school << 8) | index}, {"ram", def},
        {"name", read_name(name_addr, name_slot_size(name_addr))}, {"nameMaxLength", name_slot_size(name_addr) - 1},
        {"fields", read_fields(def, spell_fields)},
    };
    if (school < player_schools) {
        uint32_t anim = spell_anim_addr(school, index);
        json stages = json::array();
        for (int s = 0; s < anim_stage_count; s++) {
            stages.push_back(read_fields(anim + s * anim_stage_stride, anim_stage_fields));
        }
        json sounds = json::array();
        for (int s = 0; s < anim_stage_count; s++) {
            sounds.push_back({ {"sound", memory.read_u8(anim + anim_sounds_offset + s * 2)},
                               {"delay", memory.read_u8(anim + anim_sounds_offset + s * 2 + 1)} });
        }
        out["anim"] = { {"ram", anim}, {"stages", stages}, {"sounds", sounds} };
    }
    return out;
}

void reply_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void reply_error(httplib::Response& res, const std::string& message, int status = 400) {
    reply_json(res, { {"error", message} }, status);
}

bool parse_body(const httplib::Request& req, httplib::Response& res, json& out) {
    out = json::parse(req.body, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        reply_error(res, "body must be a JSON object");
        return false;
    }
    return true;
}

// Kept next to the Controller Pak files (the working directory), since that's where Quest 64's saves live.
std::filesystem::path save_slots_dir() {
    return std::filesystem::current_path() / "devpanel_slots";
}

// Quest 64 saves to a Controller Pak. The runtime writes the pak as controllerPak_*.sav files in the
// working directory (not the saves folder), so a save slot is a folder holding copies of those, plus
// the runtime's regular save file if one exists.
std::vector<std::filesystem::path> live_save_files() {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path(), ec)) {
        std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.rfind("controllerPak_", 0) == 0 && entry.path().extension() == ".sav") {
            files.push_back(entry.path());
        }
    }
    if (std::filesystem::exists(ultramodern::get_save_file_path(), ec)) {
        files.push_back(ultramodern::get_save_file_path());
    }
    return files;
}

bool copy_live_saves_to(const std::filesystem::path& slot, std::string& error) {
    std::vector<std::filesystem::path> files = live_save_files();
    if (files.empty()) {
        error = "no save files yet; save in-game first";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove_all(slot, ec);
    std::filesystem::create_directories(slot, ec);
    for (const auto& file : files) {
        std::filesystem::copy_file(file, slot / file.filename(), std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "copy failed: " + ec.message();
            return false;
        }
    }
    return true;
}

bool valid_slot_name(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (char c : name) {
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

void register_routes(httplib::Server& srv) {
    // Only answer requests addressed to localhost, and require a custom header on API calls.
    // A custom header can't be sent cross-origin without a CORS preflight, which this server never
    // approves, so other websites open in the browser can't drive the panel.
    srv.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        std::string host = req.get_header_value("Host");
        std::string hostname = host.substr(0, host.rfind(':'));
        if (hostname != "127.0.0.1" && hostname != "localhost") {
            res.status = 403;
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.path.rfind("/api/", 0) == 0 && req.get_header_value("X-Q64-Dev") != "1") {
            reply_error(res, "missing X-Q64-Dev header", 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    srv.set_mount_point("/", "./assets/devpanel");

    srv.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        reply_json(res, {
            {"gameStarted", ultramodern::is_game_started()},
            {"spellTablesLoaded", spell_tables_loaded()},
            {"saveFile", path_string(ultramodern::get_save_file_path())},
        });
    });

    srv.Get("/api/schema", [](const httplib::Request&, httplib::Response& res) {
        reply_json(res, {
            {"spellFields", fields_schema(spell_fields)},
            {"animStageFields", fields_schema(anim_stage_fields)},
            {"brianFields", fields_schema(brian_fields)},
        });
    });

    srv.Get("/api/spells", [](const httplib::Request&, httplib::Response& res) {
        if (!spell_tables_loaded()) {
            reply_error(res, "spell tables not loaded yet; start the game first", 409);
            return;
        }
        json out = json::array();
        for (int school = 0; school < spell_schools; school++) {
            for (int index = 0; index < spell_count(school); index++) {
                out.push_back(spell_json(school, index));
            }
        }
        reply_json(res, out);
    });

    // Body: { school, index, fields?: {...}, stages?: [{...} x4], sounds?: [{sound, delay} x4], name? }
    srv.Post("/api/spell", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) {
            return;
        }
        if (!spell_tables_loaded()) {
            reply_error(res, "spell tables not loaded yet", 409);
            return;
        }
        int school = body.value("school", -1);
        int index = body.value("index", -1);
        if (school < 0 || school >= spell_schools || index < 0 || index >= spell_count(school)) {
            reply_error(res, "bad school/index");
            return;
        }
        std::string error;
        if (body.contains("fields") && !write_fields(spell_def_addr(school, index), spell_fields, body["fields"], error)) {
            reply_error(res, error);
            return;
        }
        if (school < player_schools) {
            uint32_t anim = spell_anim_addr(school, index);
            if (body.contains("stages")) {
                const json& stages = body["stages"];
                for (int s = 0; s < anim_stage_count && s < (int)stages.size(); s++) {
                    if (!write_fields(anim + s * anim_stage_stride, anim_stage_fields, stages[s], error)) {
                        reply_error(res, error);
                        return;
                    }
                }
            }
            if (body.contains("sounds")) {
                const json& sounds = body["sounds"];
                for (int s = 0; s < anim_stage_count && s < (int)sounds.size(); s++) {
                    if (sounds[s].contains("sound")) {
                        memory.write_u8(anim + anim_sounds_offset + s * 2, (uint8_t)sounds[s]["sound"].get<int>());
                    }
                    if (sounds[s].contains("delay")) {
                        memory.write_u8(anim + anim_sounds_offset + s * 2 + 1, (uint8_t)sounds[s]["delay"].get<int>());
                    }
                }
            }
        }
        if (body.contains("name")) {
            std::string name = body["name"].get<std::string>();
            uint32_t name_addr = memory.read_u32(spell_name_table + name_index(school, index) * 4);
            uint32_t slot = name_slot_size(name_addr);
            if (name.size() >= slot) {
                reply_error(res, "name too long; max " + std::to_string(slot - 1) + " characters");
                return;
            }
            for (uint32_t i = 0; i < slot; i++) {
                memory.write_u8(name_addr + i, i < name.size() ? (uint8_t)name[i] : 0);
            }
        }
        reply_json(res, spell_json(school, index));
    });

    srv.Get("/api/brian", [](const httplib::Request&, httplib::Response& res) {
        reply_json(res, read_fields(0, brian_fields));
    });

    srv.Post("/api/brian", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) {
            return;
        }
        std::string error;
        if (!write_fields(0, brian_fields, body, error)) {
            reply_error(res, error);
            return;
        }
        reply_json(res, read_fields(0, brian_fields));
    });

    // Raw memory access for research. GET /api/mem?addr=0x800C06A0&len=68
    srv.Get("/api/mem", [](const httplib::Request& req, httplib::Response& res) {
        uint32_t addr = (uint32_t)std::stoul(req.get_param_value("addr"), nullptr, 0);
        uint32_t len = req.has_param("len") ? (uint32_t)std::stoul(req.get_param_value("len"), nullptr, 0) : 16;
        if (len > 0x10000 || !GameMemory::valid(addr, len)) {
            reply_error(res, "address range out of bounds");
            return;
        }
        static const char digits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(len * 2);
        for (uint32_t i = 0; i < len; i++) {
            uint8_t b = memory.read_u8(addr + i);
            hex += digits[b >> 4];
            hex += digits[b & 0xF];
        }
        reply_json(res, { {"addr", addr}, {"hex", hex} });
    });

    // Body: { addr, hex }
    srv.Post("/api/mem", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) {
            return;
        }
        uint32_t addr = body.value("addr", 0u);
        std::string hex = body.value("hex", std::string{});
        if (hex.size() % 2 != 0 || !GameMemory::valid(addr, (uint32_t)hex.size() / 2)) {
            reply_error(res, "bad addr/hex");
            return;
        }
        for (size_t i = 0; i < hex.size() / 2; i++) {
            memory.write_u8(addr + (uint32_t)i, (uint8_t)std::stoul(hex.substr(i * 2, 2), nullptr, 16));
        }
        reply_json(res, { {"ok", true} });
    });

    // Hold controller input for a while, for scripted tests:
    // POST /api/input {"buttons": ["A"], "x": 0.0, "y": 1.0, "ms": 300}  (x/y: stick, -1..1; up is +y)
    srv.Post("/api/input", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) {
            return;
        }
        static const std::pair<const char*, uint16_t> names[] = {
            {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000},
            {"DUP", 0x0800}, {"DDOWN", 0x0400}, {"DLEFT", 0x0200}, {"DRIGHT", 0x0100},
            {"L", 0x0020}, {"R", 0x0010}, {"CUP", 0x0008}, {"CDOWN", 0x0004}, {"CLEFT", 0x0002}, {"CRIGHT", 0x0001},
        };
        uint16_t buttons = 0;
        for (const auto& name : body.value("buttons", json::array())) {
            bool found = false;
            for (const auto& [key, mask] : names) {
                if (name.is_string() && name.get<std::string>() == key) {
                    buttons |= mask;
                    found = true;
                }
            }
            if (!found) {
                reply_error(res, "unknown button; use A B Z START DUP DDOWN DLEFT DRIGHT L R CUP CDOWN CLEFT CRIGHT");
                return;
            }
        }
        int ms = std::clamp(body.value("ms", 100), 0, 10000);
        std::lock_guard lock{ injected_mutex };
        injected.buttons = buttons;
        injected.x = std::clamp(body.value("x", 0.0f), -1.0f, 1.0f);
        injected.y = std::clamp(body.value("y", 0.0f), -1.0f, 1.0f);
        injected.until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        reply_json(res, { {"ok", true} });
    });

    srv.Get("/api/saves", [](const httplib::Request&, httplib::Response& res) {
        json slots = json::array();
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(save_slots_dir(), ec)) {
            if (entry.is_directory()) {
                // file_clock's epoch differs by platform; shift into system_clock (Unix epoch) portably.
                auto file_time = std::filesystem::last_write_time(entry.path(), ec);
                auto time = std::chrono::system_clock::now() +
                            std::chrono::duration_cast<std::chrono::system_clock::duration>(file_time - std::filesystem::file_time_type::clock::now());
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
                slots.push_back({ {"name", entry.path().filename().string()}, {"modified", seconds} });
            }
        }
        json files = json::array();
        for (const auto& file : live_save_files()) {
            files.push_back(path_string(file));
        }
        reply_json(res, { {"files", files}, {"slots", slots} });
    });

    // Body: { name }. Copies the current save files into a slot.
    srv.Post("/api/saves/backup", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) {
            return;
        }
        std::string name = body.value("name", std::string{});
        if (!valid_slot_name(name) || name[0] == '_') {
            reply_error(res, "slot names use letters, digits, _ and - only, and can't start with _");
            return;
        }
        std::string error;
        if (!copy_live_saves_to(save_slots_dir() / name, error)) {
            reply_error(res, error, 500);
            return;
        }
        reply_json(res, { {"ok", true} });
    });

    // Body: { name }. Backs up the current saves as "_before_restore", then replaces them with the
    // slot's files. The game picks them up the next time a file is loaded from its menu.
    srv.Post("/api/saves/restore", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) {
            return;
        }
        std::string name = body.value("name", std::string{});
        std::filesystem::path slot = save_slots_dir() / name;
        std::error_code ec;
        if (!valid_slot_name(name) || !std::filesystem::is_directory(slot, ec)) {
            reply_error(res, "no such slot");
            return;
        }
        std::string error;
        std::vector<std::filesystem::path> current = live_save_files();
        if (!current.empty() && name != "_before_restore" && !copy_live_saves_to(save_slots_dir() / "_before_restore", error)) {
            reply_error(res, error, 500);
            return;
        }
        // Remove pak files the slot doesn't have, so a restored pak never mixes files from two saves.
        for (const auto& file : current) {
            if (!std::filesystem::exists(slot / file.filename(), ec) && file != ultramodern::get_save_file_path()) {
                std::filesystem::remove(file, ec);
            }
        }
        bool restored_runtime_save = false;
        for (const auto& entry : std::filesystem::directory_iterator(slot, ec)) {
            bool is_runtime_save = entry.path().filename() == ultramodern::get_save_file_path().filename();
            std::filesystem::path target = is_runtime_save ? ultramodern::get_save_file_path()
                                                           : std::filesystem::current_path() / entry.path().filename();
            std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                reply_error(res, "copy failed: " + ec.message(), 500);
                return;
            }
            restored_runtime_save |= is_runtime_save;
        }
        if (restored_runtime_save) {
            // Re-read the file into the runtime's save buffer (same file name, so this is just a reload).
            ultramodern::change_save_file(u8"", ultramodern::get_save_file_path().stem().u8string());
        }
        reply_json(res, { {"ok", true} });
    });
}

} // namespace

bool injected_input(uint16_t* buttons, float* x, float* y) {
    std::lock_guard lock{ injected_mutex };
    if (std::chrono::steady_clock::now() >= injected.until) {
        return false;
    }
    *buttons = injected.buttons;
    *x = injected.x;
    *y = injected.y;
    return true;
}

bool requested(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (std::string_view{ argv[i] } == "--dev-panel") {
            return true;
        }
    }
    const char* env = std::getenv("Q64_DEV_PANEL");
    return env != nullptr && std::string_view{ env } == "1";
}

void start(uint8_t* rdram) {
    if (server) {
        return;
    }
    int port = default_port;
    if (const char* env = std::getenv("Q64_DEV_PANEL_PORT")) {
        port = std::atoi(env);
    }
    memory.set_rdram(rdram);
    server = std::make_unique<httplib::Server>();
    register_routes(*server);
    if (!server->bind_to_port("127.0.0.1", port)) {
        ultramodern::error_handling::message_box(
            ("Dev panel: port " + std::to_string(port) + " is already in use. "
             "Set Q64_DEV_PANEL_PORT to another port to use the dev panel.").c_str());
        server.reset();
        return;
    }
    server_thread = std::thread{ []() {
        ultramodern::set_native_thread_name("Dev Panel");
        server->listen_after_bind();
    } };
    printf("[devpanel] http://127.0.0.1:%d\n", port);
}

void stop() {
    if (server) {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        server.reset();
    }
}

} // namespace q64::devpanel
