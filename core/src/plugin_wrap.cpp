// File/layer: core/src/plugin_wrap.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginWrapTest (test_signals); FUZZ: fuzz_plugin_wrap
//
// Making and changing a packed plugin (ADR-0064, plugin.h): a signal
// file wrapped into a plugin of one variant, a variant put into a packed
// plugin or dropped from it. Each call checks its inputs through their
// own loaders, edits the parsed text and writes it with json.h's writer,
// then gives the result only when ecuio_plugin_load takes it, so the
// loaders stay the one place a rule lives.

#include "ecuio/plugin.h"
#include "ecuio/signal_sets.h"
#include "ecu_join.h"
#include "error_detail.h"
#include "json.h"
#include "least_core.h"
#include "plugin_rules.h"
#include "sha256.h"
#include "signal_file_build.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using ecuio_ecus::address;
using ecuio_ecus::ecu;
using ecuio_ecus::ecus_of;
using ecuio_ecus::fallback_of;
using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;
using ecuio_sf::q;

/* A rule of the call refused, in words */
struct refusal {
    std::string words;
};

/* A loader's refusal passed on: its words are in the detail already */
struct passed {
    ecuio_status_t status;
};

Value text_value(const std::string& s)
{
    Value v;
    v.type = Type::string;
    v.text = s;
    return v;
}

Value whole_value(int64_t n)
{
    Value v;
    v.type = Type::number;
    v.integer = true;
    v.in_int64 = true;
    v.whole = n;
    v.number = static_cast<double>(n);
    return v;
}

Value empty_of(Type t)
{
    Value v;
    v.type = t;
    return v;
}

void add(Value& object, const std::string& key, Value value)
{
    Member m;
    m.key = key;
    m.value = std::move(value);
    object.members.push_back(std::move(m));
}

Value* at(Value& object, const char* key)
{
    for (Member& m : object.members) {
        if (m.key == key) {
            return &m.value;
        }
    }
    return nullptr;
}

/* object's member key, taken out of it; null when it has none */
Value take(Value& object, const char* key)
{
    for (size_t i = 0; i < object.members.size(); ++i) {
        if (object.members[i].key == key) {
            Value v = std::move(object.members[i].value);
            object.members.erase(object.members.begin() +
                                 static_cast<std::ptrdiff_t>(i));
            return v;
        }
    }
    return Value();
}

/* Bytes a loader has taken, as their values: only memory can fail it */
Value parsed(const char* bytes, size_t length, size_t max_bytes)
{
    ecuio_json::Limits limits;
    limits.max_bytes = max_bytes;
    ecuio_json::Error error;
    Value root;
    if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length, &root, &error,
                           limits)) {
        throw std::bad_alloc();
    }
    return root;
}

void check_signal_file(const char* file, size_t length)
{
    ecuio_signal_file_t* f = nullptr;
    const ecuio_status_t s = ecuio_signal_file_load(file, length, &f);
    ecuio_signal_file_free(f);
    if (s != ECUIO_OK) {
        throw passed{s};
    }
}

/* prefix goes before the loader's words: which input they place */
void check_packed(const char* packed, size_t length, const char* prefix)
{
    ecuio_plugin_t* p = nullptr;
    const ecuio_status_t s =
        ecuio_plugin_load(packed, length, nullptr, nullptr, &p);
    if (s != ECUIO_OK) {
        const std::string words = ecuio_last_error_detail();
        ecuio_detail_set("%s%s", prefix, words.c_str());
        throw passed{s};
    }
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    (void)ecuio_plugin_info(p, &info);
    ecuio_plugin_free(p);
    if ((info.tiers & ECUIO_PLUGIN_TIER_CONVERSATION) != 0) {
        throw refusal{"the plugin has the conversation tier, which a put or "
                      "a drop doesn't carry over: change the variant in the "
                      "plugin's folder, which Havne loads as it stands"};
    }
}

/* Whether ecuio_plugin_load takes text; when not, *why is its reason
 * without the place, which is in a text the caller never saw */
bool loads(const std::string& text, std::string* why)
{
    ecuio_plugin_t* p = nullptr;
    const ecuio_status_t s =
        ecuio_plugin_load(text.data(), text.size(), nullptr, nullptr, &p);
    if (s == ECUIO_OK) {
        ecuio_plugin_free(p);
        return true;
    }
    if (s == ECUIO_ERR_OUT_OF_MEMORY) {
        throw std::bad_alloc();
    }
    const std::string words = ecuio_last_error_detail();
    const size_t colon = words.find(": ");
    *why = words.compare(0, 5, "line ") == 0 && colon != std::string::npos
               ? words.substr(colon + 2)
               : words;
    return false;
}

/* data/<variant>.json, numbered from -2 while files holds it or it names
 * a Windows device: the package laid out as a folder, the form a plugin
 * is edited in, would open the device */
std::string data_path(const std::string& variant, const Value* files)
{
    std::string path = "data/" + variant + ".json";
    for (unsigned n = 2; ecuio_plugin_rules::names_a_device(path) ||
                         (files != nullptr && files->find(path) != nullptr);
         ++n) {
        path = "data/" + variant + "-" + std::to_string(n) + ".json";
    }
    return path;
}

/* name as user.<slug>'s slug, cut to cap bytes with no '-' at its end */
std::string slug_of(const std::string& name, size_t cap)
{
    std::string s = ecuio_sf::profile_of(name);
    s.erase(0, std::strlen("user."));
    if (s.size() > cap) {
        s.resize(cap);
    }
    while (!s.empty() && s.back() == '-') {
        s.pop_back();
    }
    return s;
}

void check_id(const std::string& id, size_t most, const char* what)
{
    if (!ecuio_plugin_rules::id_ok(id, most)) {
        throw refusal{std::string(what) + " " + q(id) + " takes 1 to " +
                      std::to_string(most) +
                      " of a-z, 0-9 and -, a letter or digit at each end"};
    }
}

/* doc's requires.core made the least core that reads doc and at least
 * floor (least_core.h): the plugin loader works it out from doc as
 * written. A doc it refuses keeps its value: the check after says why. */
void set_least_core(Value& doc, ecuio_least::version floor)
{
    const std::string text = ecuio_json::write(doc);
    ecuio_plugin_t* p = nullptr;
    const ecuio_status_t s =
        ecuio_plugin_load(text.data(), text.size(), nullptr, nullptr, &p);
    if (s == ECUIO_ERR_OUT_OF_MEMORY) {
        throw std::bad_alloc();
    }
    if (s != ECUIO_OK) {
        return;
    }
    ecuio_least::raise(floor, ecuio_least::of_plugin(p));
    ecuio_plugin_free(p);
    at(*at(doc, "requires"), "core")->text =
        ecuio_least::text(ecuio_least::within_core(floor));
}

/* A changed doc's requires.core: the least core that reads it, or the
 * value it had when that is higher, which is the author's word and
 * keeps the output a function of the inputs */
void keep_or_raise_core(Value& doc)
{
    ecuio_least::version stored = {{0, 0, 0}};
    if (!ecuio_plugin_rules::parse_version(
            at(*at(doc, "requires"), "core")->text, stored.v)) {
        stored = ecuio_least::version{{0, 0, 0}};
    }
    set_least_core(doc, stored);
}

std::string wrapped(const char* file, size_t length,
                    const ecuio_plugin_wrap_options_t* options)
{
    const char* id_given = nullptr;
    const char* variant_given = nullptr;
    const char* wmi = nullptr;
    uint32_t years[2] = {0, 0};
    if (options != nullptr) {
        if (options->struct_size < sizeof(ecuio_plugin_wrap_options_t)) {
            throw refusal{"the options' struct_size is smaller than the "
                          "options"};
        }
        bool spare = options->flags != 0;
        for (uint32_t r : options->reserved) {
            spare = spare || r != 0;
        }
        if (spare) {
            throw refusal{"the options' flags and reserved words are zero"};
        }
        id_given = options->id;
        variant_given = options->variant;
        wmi = options->wmi;
        years[0] = options->years[0];
        years[1] = options->years[1];
    }
    if (wmi == nullptr && (years[0] != 0 || years[1] != 0)) {
        throw refusal{"model years go with manufacturer codes: a match "
                      "needs its wmi"};
    }

    check_signal_file(file, length);
    Value in = parsed(file, length, ECUIO_SIGNAL_FILE_MAX_BYTES);
    const std::string name = in.find("name")->text;
    const std::string id =
        id_given != nullptr ? id_given : slug_of(name, ECUIO_PLUGIN_ID_MAX);
    check_id(id, ECUIO_PLUGIN_ID_MAX, "the plugin's id");
    const std::string variant =
        variant_given != nullptr ? variant_given : "base";
    check_id(variant, ECUIO_PLUGIN_VARIANT_ID_MAX, "the variant's id");

    Value doc = empty_of(Type::object);
    add(doc, "havne_plugin", whole_value(ECUIO_PLUGIN_FORMAT));
    add(doc, "id", text_value(id));
    add(doc, "name", text_value(name));
    add(doc, "version", text_value("1.0.0"));
    Value needs = empty_of(Type::object);
    add(needs, "core", text_value("0.0.0")); /* set_least_core's */
    add(doc, "requires", std::move(needs));
    add(doc, "lineage",
        text_value(ecuio_sha256::hex_of(file, length)
                       .substr(0, ECUIO_PLUGIN_LINEAGE_LEN)));
    add(doc, "source", take(in, "source"));
    Value tiers = empty_of(Type::array);
    tiers.items.push_back(text_value("data"));
    add(doc, "tiers", std::move(tiers));
    if (wmi != nullptr) {
        Value codes = empty_of(Type::array);
        const std::string list = wmi;
        size_t start = 0;
        for (size_t i = 0; i <= list.size(); ++i) {
            if (i == list.size() || list[i] == ',') {
                codes.items.push_back(
                    text_value(list.substr(start, i - start)));
                start = i + 1;
            }
        }
        Value rule = empty_of(Type::object);
        add(rule, "wmi", std::move(codes));
        if (years[0] != 0 || years[1] != 0) {
            Value range = empty_of(Type::array);
            range.items.push_back(whole_value(years[0]));
            range.items.push_back(whole_value(years[1]));
            add(rule, "years", std::move(range));
        }
        Value match = empty_of(Type::array);
        match.items.push_back(std::move(rule));
        add(doc, "match", std::move(match));
    }
    if (in.find("ecus") != nullptr) {
        add(doc, "ecus", take(in, "ecus"));
    }
    const std::string path = data_path(variant, nullptr);
    Value entry = empty_of(Type::object);
    add(entry, "id", text_value(variant));
    Value listed = empty_of(Type::array);
    listed.items.push_back(text_value(path));
    add(entry, "files", std::move(listed));
    Value variants = empty_of(Type::array);
    variants.items.push_back(std::move(entry));
    add(doc, "variants", std::move(variants));
    Value files = empty_of(Type::object);
    add(files, path, std::move(in));
    add(doc, "files", std::move(files));
    set_least_core(doc, ecuio_least::version{{0, 0, 0}});
    return ecuio_json::write(doc);
}

/* A signal that reads a reply and names no ECU */
bool unnamed(const Value& signal)
{
    const Value* kind = signal.find("kind");
    return signal.find("ecu") == nullptr &&
           (kind == nullptr || kind->text != "computed");
}

/* The manifest's ECU ids that file's signals read, first the id one
 * naming none falls to ("" with no manifest ECU) */
void add_read(const Value& file, const std::string& first,
              std::set<std::string>* ids)
{
    for (const Value& s : file.find("signals")->items) {
        if (const Value* named = s.find("ecu")) {
            ids->insert(named->text);
        } else if (unnamed(s) && !first.empty()) {
            ids->insert(first);
        }
    }
}

struct package {
    Value doc;

    Value& variants() { return *at(doc, "variants"); }
    Value& files() { return *at(doc, "files"); }

    /* The manifest's "ecus", made before "variants" when it has none */
    Value& ecus()
    {
        if (Value* have = at(doc, "ecus")) {
            return *have;
        }
        size_t place = 0;
        while (doc.members[place].key != "variants") {
            ++place;
        }
        Member m;
        m.key = "ecus";
        m.value = empty_of(Type::array);
        doc.members.insert(doc.members.begin() +
                               static_cast<std::ptrdiff_t>(place),
                           std::move(m));
        return doc.members[place].value;
    }

    /* The variant's place in "variants", or their count */
    size_t find(const std::string& id)
    {
        Value& list = variants();
        size_t i = 0;
        while (i < list.items.size() && list.items[i].find("id")->text != id) {
            ++i;
        }
        return i;
    }

    /* Whether path is one of the manifest's "pages", which no variant reads */
    bool is_page(const std::string& path)
    {
        if (const Value* list = at(doc, "pages")) {
            for (const Value& f : list->items) {
                if (f.text == path) {
                    return true;
                }
            }
        }
        return false;
    }

    bool listed_elsewhere(const std::string& path, size_t but)
    {
        Value& list = variants();
        for (size_t i = 0; i < list.items.size(); ++i) {
            if (i == but) {
                continue;
            }
            for (const Value& f : list.items[i].find("files")->items) {
                if (f.text == path) {
                    return true;
                }
            }
        }
        return false;
    }

    /* Takes out the data files only variant index lists, and the ECUs
     * only those files read; the pages stay */
    void detach(size_t index)
    {
        const Value* list = at(doc, "ecus");
        const std::string first = list != nullptr && !list->items.empty()
                                      ? list->items.front().find("id")->text
                                      : std::string();
        std::set<std::string> gone;
        std::set<std::string> kept;
        std::vector<Member> staying;
        for (Member& m : files().members) {
            if (is_page(m.key)) {
                staying.push_back(std::move(m));
                continue;
            }
            bool own = false;
            for (const Value& f :
                 variants().items[index].find("files")->items) {
                own = own || f.text == m.key;
            }
            if (own && !listed_elsewhere(m.key, index)) {
                add_read(m.value, first, &gone);
            } else {
                add_read(m.value, first, &kept);
                staying.push_back(std::move(m));
            }
        }
        files().members = std::move(staying);
        if (Value* ecus_v = at(doc, "ecus")) {
            std::vector<Value> left;
            for (Value& e : ecus_v->items) {
                const std::string& id = e.find("id")->text;
                if (gone.count(id) == 0 || kept.count(id) != 0) {
                    left.push_back(std::move(e));
                }
            }
            ecus_v->items = std::move(left);
        }
    }

    /* "X.Y.Z" to "X.(Y+1).0" */
    void raise_version()
    {
        Value& version = *at(doc, "version");
        uint32_t n[3] = {0, 0, 0};
        ecuio_plugin_rules::parse_version(version.text, n);
        if (n[1] >= 65535) {
            throw refusal{"version " + version.text +
                          " has no minor left to raise: 65535 is the most"};
        }
        version.text =
            std::to_string(n[0]) + "." + std::to_string(n[1] + 1) + ".0";
    }
};

/* A put's work on the manifest's ECUs and the signals that name them */
class ecu_merge : private ecuio_ecus::joined
{
public:
    /* have_ is the manifest's ECUs, then those added */
    ecu_merge(package& p, Value& file)
        : joined(ecus_of(at(p.doc, "ecus"))), p_(p), file_(file)
    {}

    void run()
    {
        const address before = fallback_of(have_);
        const std::vector<ecu> theirs = ecus_of(file_.find("ecus"));
        const address own = fallback_of(theirs);
        const Value own_list = take(file_, "ecus");
        std::map<std::string, std::string> id_here;
        for (size_t i = 0; i < theirs.size(); ++i) {
            const ecu& t = theirs[i];
            const ecu* same = at_address(t.addr);
            if (same != nullptr) {
                id_here[t.id] = same->id;
                keep_name(own_list.items[i], same->id);
                if (same->id != t.id) {
                    notes_.push_back("the file's ecu " + q(t.id) +
                                     " is the plugin's " + q(same->id));
                }
                continue;
            }
            std::string id = t.id;
            if (taken(id)) {
                id = free_id(t.id);
                notes_.push_back("the file's ecu " + q(t.id) + " is " + q(id) +
                                 " in the plugin, whose " + q(t.id) +
                                 " has other addresses");
            }
            Value entry = own_list.items[i];
            at(entry, "id")->text = id;
            append(std::move(entry), id, t.addr);
            id_here[t.id] = id;
        }
        const address after = fallback_of(have_);

        for (Value& s : at(file_, "signals")->items) {
            if (Value* named = at(s, "ecu")) {
                named->text = id_here[named->text];
            } else if (unnamed(s) && !(own == after)) {
                add(s, "ecu", text_value(id_at(own)));
            }
        }
        if (!(before == after)) {
            for (Member& m : p_.files().members) {
                if (p_.is_page(m.key)) {
                    continue;
                }
                for (Value& s : at(m.value, "signals")->items) {
                    if (unnamed(s)) {
                        add(s, "ecu", text_value(id_at(before)));
                    }
                }
            }
        }
        if (!added_.empty()) {
            Value& list = p_.ecus();
            for (Value& entry : added_) {
                list.items.push_back(std::move(entry));
            }
        }
    }

    /* What the user is told: "" when no ECU of the file took another id.
     * Notes join while they fit the detail, then "and N more" ends it. */
    std::string notes() const
    {
        const auto more = [](const std::string& head, size_t n) {
            return (head.empty() ? "" : "; ") + std::string("and ") +
                   std::to_string(n) + " more";
        };
        std::string all;
        for (size_t i = 0; i < notes_.size(); ++i) {
            std::string next = all + (all.empty() ? "" : "; ") + notes_[i];
            const size_t left = notes_.size() - i - 1;
            const size_t tail = left == 0 ? 0 : more(next, left).size();
            if (next.size() + tail > ecuio_detail_max) {
                return all + more(all, notes_.size() - i);
            }
            all = std::move(next);
        }
        return all;
    }

private:
    /* The file's name for a module the manifest's entry leaves unnamed */
    void keep_name(const Value& theirs, const std::string& id)
    {
        const Value* name = theirs.find("name");
        Value* list = at(p_.doc, "ecus");
        if (name == nullptr || list == nullptr) {
            return;
        }
        for (Value& e : list->items) {
            if (e.find("id")->text == id && e.find("name") == nullptr) {
                add(e, "name", *name);
            }
        }
    }

    void append(Value entry, const std::string& id, const address& a)
    {
        added_.push_back(std::move(entry));
        ecu e;
        e.id = id;
        e.addr = a;
        have_.push_back(std::move(e));
    }

    /* The manifest's id for the ECU at a, added as "obd2" when it has
     * none there: only the extended set's first ECU can lack an entry */
    std::string id_at(const address& a)
    {
        if (const ecu* e = at_address(a)) {
            return e->id;
        }
        const std::string id = taken("obd2") ? free_id("obd2") : "obd2";
        char hex[16];
        Value entry = empty_of(Type::object);
        add(entry, "id", text_value(id));
        std::snprintf(hex, sizeof hex, "%X", static_cast<unsigned>(a.rx));
        add(entry, "rx", text_value(hex));
        std::snprintf(hex, sizeof hex, "%X", static_cast<unsigned>(a.tx));
        add(entry, "tx", text_value(hex));
        append(std::move(entry), id, a);
        return id;
    }

    package& p_;
    Value& file_;
    std::vector<Value> added_; /* joined to the manifest at the end */
    std::vector<std::string> notes_;
};

std::string put(const char* packed, size_t length, const char* variant_given,
                const char* file, size_t file_length, std::string* notes)
{
    check_packed(packed, length, "the plugin, ");
    check_signal_file(file, file_length);
    package p;
    p.doc = parsed(packed, length, ECUIO_PLUGIN_PACKAGE_MAX);
    Value in = parsed(file, file_length, ECUIO_SIGNAL_FILE_MAX_BYTES);
    const std::string variant =
        variant_given != nullptr
            ? variant_given
            : slug_of(in.find("name")->text, ECUIO_PLUGIN_VARIANT_ID_MAX);
    check_id(variant, ECUIO_PLUGIN_VARIANT_ID_MAX, "the variant's id");

    const size_t index = p.find(variant);
    const bool replaces = index < p.variants().items.size();
    if (replaces) {
        p.detach(index);
    }
    ecu_merge merge(p, in);
    merge.run();
    *notes = merge.notes();

    const std::string path = data_path(variant, &p.files());
    add(p.files(), path, std::move(in));
    Value listed = empty_of(Type::array);
    listed.items.push_back(text_value(path));
    if (replaces) {
        *at(p.variants().items[index], "files") = std::move(listed);
    } else {
        Value entry = empty_of(Type::object);
        add(entry, "id", text_value(variant));
        add(entry, "files", std::move(listed));
        p.variants().items.push_back(std::move(entry));
    }
    p.raise_version();
    keep_or_raise_core(p.doc);
    return ecuio_json::write(p.doc);
}

std::string dropped(const char* packed, size_t length, const char* variant)
{
    check_packed(packed, length, "");
    package p;
    p.doc = parsed(packed, length, ECUIO_PLUGIN_PACKAGE_MAX);
    const size_t index = p.find(variant);
    if (index == p.variants().items.size()) {
        throw refusal{"the plugin has no variant " + q(variant)};
    }
    if (p.variants().items.size() == 1) {
        throw refusal{"variant " + q(variant) +
                      " is the plugin's only one, and a plugin keeps one"};
    }
    p.detach(index);
    p.variants().items.erase(p.variants().items.begin() +
                             static_cast<std::ptrdiff_t>(index));
    p.raise_version();
    /* The version is in each variant's written copy, so a longer one
     * can carry a copy past a file's bytes */
    keep_or_raise_core(p.doc);
    return ecuio_json::write(p.doc);
}

/* text as the caller's to free, or bad_alloc */
void give(const std::string& text, char** out, size_t* out_length)
{
    char* copy = static_cast<char*>(std::malloc(text.size() + 1));
    if (copy == nullptr) {
        throw std::bad_alloc();
    }
    std::memcpy(copy, text.c_str(), text.size() + 1);
    *out = copy;
    if (out_length != nullptr) {
        *out_length = text.size();
    }
}

/* One call's body run to a status: make(&notes) gives the text, and
 * advice() what to add to the loader's refusal of it; missing says an
 * input the call needs is NULL; nothing thrown crosses into the caller */
template <typename Make, typename Advice>
ecuio_status_t answer(bool missing, char** out, size_t* out_length, Make make,
                      Advice advice)
{
    ecuio_detail_clear();
    if (out_length != nullptr) {
        *out_length = 0;
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    if (out == nullptr || missing) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    try {
        std::string notes;
        const std::string text = make(&notes);
        std::string why;
        if (!loads(text, &why)) {
            throw refusal{why + advice()};
        }
        give(text, out, out_length);
        ecuio_detail_clear();
        if (!notes.empty()) {
            ecuio_detail_set("%s", notes.c_str());
        }
        return ECUIO_OK;
    } catch (const passed& p) {
        return p.status;
    } catch (const refusal& r) {
        ecuio_detail_set("%s", r.words.c_str());
        return ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the plugin couldn't be written");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

} // namespace

extern "C" {

ecuio_status_t ecuio_plugin_wrap(const char* file, size_t length,
                                 const ecuio_plugin_wrap_options_t* options,
                                 char** out, size_t* out_length)
{
    return answer(
        file == nullptr && length > 0, out, out_length,
        [&](std::string*) { return wrapped(file, length, options); },
        [] { return std::string(); });
}

ecuio_status_t ecuio_plugin_put_variant(const char* packed, size_t length,
                                        const char* variant, const char* file,
                                        size_t file_length, char** out,
                                        size_t* out_length)
{
    return answer(
        (packed == nullptr && length > 0) ||
            (file == nullptr && file_length > 0),
        out, out_length,
        [&](std::string* notes) {
            return put(packed, length, variant, file, file_length, notes);
        },
        [&] {
            /* Said only when it holds: the file alone makes a plugin */
            std::string why;
            return loads(wrapped(file, file_length, nullptr), &why)
                       ? std::string("; the file can be a plugin of its own")
                       : std::string();
        });
}

ecuio_status_t ecuio_plugin_drop_variant(const char* packed, size_t length,
                                         const char* variant, char** out,
                                         size_t* out_length)
{
    return answer(
        (packed == nullptr && length > 0) || variant == nullptr, out,
        out_length,
        [&](std::string*) { return dropped(packed, length, variant); },
        [] { return std::string(); });
}

void ecuio_plugin_text_free(char* text)
{
    std::free(text);
}

} // extern "C"
