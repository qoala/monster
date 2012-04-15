/*
 * ===========================================================================
 * Copyright (C) 2007 Marc H. Thoben
 * Copyright (C) 2008 Darshan Shaligram
 * Copyright (C) 2010 Jude Brown
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * ===========================================================================
 */

#include "AppHdr.h"
#include "externs.h"
#include "unwind.h"
#include "env.h"
#include "colour.h"
#include "dungeon.h"
#include "los.h"
#include "message.h"
#include "mon-abil.h"
#include "mon-cast.h"
#include "mon-util.h"
#include "version.h"
#include "view.h"
#include "los.h"
#include "maps.h"
#include "initfile.h"
#include "libutil.h"
#include "itemname.h"
#include "mon-stuff.h"
#include "random.h"
#include "spl-util.h"
#include "state.h"
#include "vault_monsters.h"
#include <sstream>
#include <set>
#include <unistd.h>

const coord_def MONSTER_PLACE(20, 20);
const coord_def PLAYER_PLACE(21, 20);

const std::string CANG = "cang";

const int PLAYER_MAXHP = 500;

// Clockwise, around the compass from north (same order as enum RUN_DIR)
const coord_def Compass[9] =
{
    coord_def(0, -1), coord_def(1, -1), coord_def(1, 0), coord_def(1, 1),
    coord_def(0, 1), coord_def(-1, 1), coord_def(-1, 0), coord_def(-1, -1),
    coord_def(0, 0),
};

bool is_element_colour(int col)
{
    col = col & 0x007f;
    ASSERT(col < NUM_COLOURS);
    return (col >= ETC_FIRE);
}


static std::string colour_codes[] = {
    "",
    "02",
    "03",
    "10",
    "05",
    "06",
    "07",
    "15",
    "14",
    "12",
    "09",
    "11",
    "04",
    "13",
    "08",
    "16"
};

static int bgr[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

#ifdef CONTROL
#undef CONTROL
#endif
#define CONTROL(x) char(x - 'A' + 1)

static std::string colour(int colour, std::string text, bool bg = false)
{
    if (is_element_colour(colour))
        colour = element_colour(colour, true);

    if (isatty(1))
    {
        if (!colour)
            return text;
        return make_stringf("\e[0;%d%d;%dm%s\e[0m", bg ? 4 : 3, bgr[colour & 7],
                            (colour & 8) ? 1 : 0, text.c_str());
    }

    const std::string code(colour_codes[colour]);

    if (code.empty())
        return text;

    return (std::string() + CONTROL('C') + code + (bg ? ",01" : "")
            + text + CONTROL('O'));
}

std::string uppercase_first(std::string s);

template <class T> inline std::string to_string (const T& t);

static void record_resvul(int color, const char *name, const char *caption,
                          std::string &str, int rval)
{
  if (str.empty())
    str = " | " + std::string(caption) + ": ";
  else
    str += ", ";

  if (color && (rval == 3 || rval == 1 && color == BROWN
                || std::string(caption) == "Vul")
            && (int) color <= 7)
    color += 8;

  std::string token(name);
  if (rval > 1 && rval <= 3) {
    while (rval-- > 0)
      token += "+";
  }

  str += colour(color, token);
}

static void record_resist(int colour, const char *name,
                          std::string &res, std::string &vul,
                          int rval)
{
  if (rval > 0)
    record_resvul(colour, name, "Res", res, rval);
  else if (rval < 0)
    record_resvul(colour, name, "Vul", vul, -rval);
}

static void monster_action_cost(std::string &qual, int cost, const char *desc) {
  char buf[80];
  if (cost != 10) {
    snprintf(buf, sizeof buf, "%s: %d%%", desc, cost * 10);
    if (!qual.empty())
      qual += "; ";
    qual += buf;
  }
}

static std::string monster_speed(const monster &mon,
                                 const monsterentry *me,
                                 int speed_min,
                                 int speed_max)
{
  std::string speed;

  char buf[50];
  if (speed_max != speed_min)
    snprintf(buf, sizeof buf, "%i-%i", speed_min, speed_max);
  else
    snprintf(buf, sizeof buf, "%i", speed_max);

  speed += buf;

  const mon_energy_usage &cost(me->energy_usage);
  std::string qualifiers;

  bool skip_action = false;
  if (cost.attack != 10
      && cost.attack == cost.missile && cost.attack == cost.spell
      && cost.attack == cost.special && cost.attack == cost.item)
  {
    monster_action_cost(qualifiers, cost.attack, "act");
    skip_action = true;
  }

  monster_action_cost(qualifiers, cost.move, "move");
  if (cost.swim != cost.move)
    monster_action_cost(qualifiers, cost.swim, "swim");
  if (!skip_action)
  {
    monster_action_cost(qualifiers, cost.attack, "atk");
    monster_action_cost(qualifiers, cost.missile, "msl");
    monster_action_cost(qualifiers, cost.spell, "spell");
    monster_action_cost(qualifiers, cost.special, "special");
    monster_action_cost(qualifiers, cost.item, "item");
  }

  if (!qualifiers.empty())
    speed += " (" + qualifiers + ")";

  return speed;
}

static void mons_flag(std::string &flag, const std::string &newflag) {
  if (flag.empty())
    flag = " | Flags: ";
  else
    flag += ", ";
  flag += newflag;
}

static void mons_check_flag(bool set, std::string &flag,
                            const std::string &newflag)
{
  if (set)
    mons_flag(flag, newflag);
}

static void initialize_crawl() {
  init_monsters();
  init_properties();
  init_item_name_cache();

  init_spell_descs();
  init_monster_symbols();
  init_mon_name_cache();
  init_spell_name_cache();
  init_mons_spells();
  init_element_colours();

  dgn_reset_level();
  for (int y = 0; y < GYM; ++y)
    for (int x = 0; x < GXM; ++x)
      grd[x][y] = DNGN_FLOOR;

  los_changed();
  you.moveto(PLAYER_PLACE);
  you.hp = you.hp_max = PLAYER_MAXHP;
}

static std::string dice_def_string(dice_def dice) {
  return (dice.num == 1? make_stringf("d%d", dice.size)
          : make_stringf("%dd%d", dice.num, dice.size));
}

static dice_def mi_calc_iood_damage(monster *mons) {
  const int power = stepdown_value(6 * mons->hit_dice,
                                   30, 30, 200, -1);
  return dice_def(8, power / 4);
}

static std::string mi_calc_smiting_damage(monster *mons) {
  return "7-17";
}

static std::string mi_calc_airstrike_damage(monster *mons) {
  return make_stringf("0-%d", 10 + 2 * mons->hit_dice);
}

static std::string mons_human_readable_spell_damage_string(
    monster *monster,
    spell_type sp)
{
  bolt spell_beam = mons_spells(monster, sp, 12 * monster->hit_dice,
                                true);
  if (sp == SPELL_SMITING)
    return make_stringf(" (%s)", mi_calc_smiting_damage(monster).c_str());
  if (sp == SPELL_AIRSTRIKE)
    return make_stringf(" (%s)", mi_calc_airstrike_damage(monster).c_str());
  if (sp == SPELL_IOOD)
    spell_beam.damage = mi_calc_iood_damage(monster);
  if (spell_beam.damage.size && spell_beam.damage.num)
    return make_stringf(" (%s)", dice_def_string(spell_beam.damage).c_str());
  return ("");
}

static std::string shorten_spell_name(std::string name) {
  lowercase(name);
  std::string::size_type pos = name.find('\'');
  if (pos != std::string::npos) {
    pos = name.find(' ', pos);
    if (pos != std::string::npos)
      name = name.substr(pos + 1);
  }
  if ((pos = name.find(" of ")) != std::string::npos)
    name = name.substr(pos + 4) + " " + name.substr(0, pos);
  if (name.find("summon ") == 0 && name != "summon undead")
    name = name.substr(7);
  if (name.find("bolt") == name.length() - 4)
    name = "b." + name.substr(0, name.length() - 5);
  return (name);
}

static void mons_record_ability(std::set<std::string> &ability_names,
                                monster *monster)
{
  no_messages mx;
  bolt beam;
  you.hp = you.hp_max = PLAYER_MAXHP;
  monster->moveto(MONSTER_PLACE);
  mon_special_ability(monster, beam);
  if (monster->pos() != MONSTER_PLACE)
    beam.name = "blink";
  if (you.hp == PLAYER_MAXHP / 2 + 1)
    beam.name = "symbol of torment";
  if (!beam.name.empty()) {
    std::string ability = shorten_spell_name(beam.name);
    if (beam.damage.num && beam.damage.size) {
      std::string extra;
      // Skip the shield slot when reckoning acid damage.
      if (ability == "acid splash")
        extra = "+" +
          dice_def_string(dice_def(EQ_MAX_ARMOUR - EQ_MIN_ARMOUR + 2, 5));
      ability += make_stringf(" (%s%s)",
                              dice_def_string(beam.damage).c_str(),
                              extra.c_str());
    }
    ability_names.insert(ability);
  }
}

static std::string mons_special_ability_set(monster *monster) {
  if (mons_genus(monster->type) == MONS_DRACONIAN
      && draco_subspecies(monster) != MONS_YELLOW_DRACONIAN)
  {
    return ("");
  }

  // Try X times to get a list of abilities.
  std::set<std::string> abilities;
  for (int i = 0; i < 50; ++i)
    mons_record_ability(abilities, monster);
  if (abilities.empty())
    return ("");
  return comma_separated_line(abilities.begin(), abilities.end(), ", ", ", ");
}

static spell_type mi_draconian_breath_spell(monster *mons) {
  if (mons_genus(mons->type) != MONS_DRACONIAN)
    return SPELL_NO_SPELL;
  switch (draco_subspecies(mons)) {
  case MONS_DRACONIAN:
  case MONS_YELLOW_DRACONIAN:
    return SPELL_NO_SPELL;
  default:
    return SPELL_DRACONIAN_BREATH;
  }
}

static std::string mons_spell_set(monster *mp) {
  std::set<spell_type> seen;
  std::string spells;

  rng_save_excursion exc(1);
  for (int i = -1; i < NUM_MONSTER_SPELL_SLOTS; ++i) {
    const spell_type sp = i == -1?
      mi_draconian_breath_spell(mp) : mp->spells[i];
    if (sp != SPELL_NO_SPELL && seen.find(sp) == seen.end()) {
      seen.insert(sp);
      std::string rawname = spell_title(sp);
      if (sp == SPELL_DRACONIAN_BREATH) {
        const bolt spell_beam = mons_spells(mp, sp, 12 * mp->hit_dice, true);
        rawname = spell_title(spell_beam.origin_spell);
      }
      std::string name = shorten_spell_name(rawname);
      if (!spells.empty())
        spells += ", ";
      spells += name + mons_human_readable_spell_damage_string(mp, sp);
    }
  }
  return spells;
}

static void record_spell_set(monster *mp,
                             std::set<std::string> &sets)
{
  std::string spell_set = mons_spell_set(mp);
  if (!spell_set.empty())
    sets.insert(spell_set);
}

static std::string mons_spells_abilities(
  monster *monster,
  bool shapeshifter,
  const std::set<std::string> &spell_sets)
{
  if (shapeshifter || monster->type == MONS_PANDEMONIUM_LORD)
    return "(random)";

  bool first = true;
  std::string spell_abilities = mons_special_ability_set(monster);
  for (std::set<std::string>::const_iterator i = spell_sets.begin();
       i != spell_sets.end(); ++i)
  {
    if (!first)
      spell_abilities += " / ";
    else if (!spell_abilities.empty())
      spell_abilities += "; ";
    first = false;
    spell_abilities += *i;
  }
  return (spell_abilities);
}

static inline void set_min_max(int num, int &min, int &max) {
  if (!min || num < min)
    min = num;
  if (!max || num > max)
    max = num;
}

static std::string monster_symbol(const monster &mon) {
  std::string symbol;
  const monsterentry *me = mon.find_monsterentry();
  if (me) {
    symbol += me->basechar;
    symbol = colour(mon.colour, symbol);
  }
  return (symbol);
}

int mi_create_monster(mons_spec spec) {
  monster *monster = 
    dgn_place_monster(spec, 10, MONSTER_PLACE, true, false, false);
  if (monster) {
    monster->behaviour = BEH_SEEK;
    monster->foe = MHITYOU;
    no_messages mx;
    monster->del_ench(ENCH_SUBMERGED);
    return monster->mindex();
  }
  return NON_MONSTER;
}

static std::string damage_flavour(const std::string &name,
                                  const std::string &damage)
{
  return "(" + name + ":" + damage + ")";
}

static std::string damage_flavour(const std::string &name,
                                  int low, int high)
{
  return make_stringf("(%s:%d-%d)", name.c_str(), low, high);
}

static void rebind_mspec(std::string *requested_name,
                         const std::string &actual_name,
                         mons_spec *mspec)
{
  if (*requested_name != actual_name
      && requested_name->find("draconian") == 0)
  {
    // If the user requested a drac, the game might generate a
    // coloured drac in response. Try to reuse that colour for further
    // tests.
    mons_list mons;
    const std::string err = mons.add_mons(actual_name, false);
    if (err.empty())
    {
      *mspec          = mons.get_monster(0);
      *requested_name = actual_name;
    }
  }
}

static std::string canned_reports[][2] = {
  { "cang",
    ("cang (" + colour(LIGHTRED, "Ω")
     + (") | Speed: c | HD: i | Health: 666 | "
        "AC/EV: e/π | Damage: 999 | Res: sanity | XP: ∞")) },
};

int main(int argc, char *argv[])
{
  crawl_state.test = true;
  if (argc < 2)
  {
    printf("Usage: @? <monster name>\n");
    return 0;
  }

  if (strstr(argv[1], "-version"))
  {
    printf("%s\n",
           std::string("Monster stats Crawl version: "
                       + Version::Long()).c_str());
    return 0;
  }

  initialize_crawl();

  mons_list mons;
  std::string target = argv[1];

  if (argc > 2)
    for (int x = 2; x < argc; x++)
    {
      target.append(" ");
      target.append(argv[x]);
    }

  trim_string(target);

  // [ds] Nobody mess with cang.
  for (unsigned i = 0; i < sizeof(canned_reports) / sizeof(*canned_reports);
       ++i)
  {
    if (canned_reports[i][0] == target)
    {
      printf("%s\n", canned_reports[i][1].c_str());
      return 0;
    }
  }

  std::string orig_target = std::string(target);

  std::string err = mons.add_mons(target, false);
  if (!err.empty()) {
    target = "the " + target;
    const std::string test = mons.add_mons(target, false);
    if (test.empty())
      err = test;
  }

  mons_spec spec = mons.get_monster(0);
  monster_type spec_type = static_cast<monster_type>(spec.type);
  bool vault_monster = false;

  if ((spec_type < 0 || spec_type >= NUM_MONSTERS
       || spec_type == MONS_PLAYER_GHOST)
      || !err.empty())
  {
    spec = get_vault_monster(orig_target);
    spec_type = static_cast<monster_type>(spec.type);
    if (spec_type < 0 || spec_type >= NUM_MONSTERS
        || spec_type == MONS_PLAYER_GHOST)
    {
      if (err.empty())
        printf("unknown monster: \"%s\"\n", target.c_str());
      else
        printf("%s\n", err.c_str());
      return 1;
    }

    vault_monster = true;
  }

  int index = mi_create_monster(spec);
  if (index < 0 || index >= MAX_MONSTERS) {
    printf("Failed to create test monster for %s\n", target.c_str());
    return 1;
  }

  const int ntrials = 1000;

  std::set<std::string> spell_sets;

  long exper = 0L;
  int hp_min = 0;
  int hp_max = 0;
  int mac = 0;
  int mev = 0;
  int speed_min = 0, speed_max = 0;
  // Calculate averages.
  for (int i = 0; i < ntrials; ++i) {
    if (i == ntrials)
      break;
    monster *mp = &menv[index];
    const std::string mname = mp->name(DESC_PLAIN, true);
    if (!mons_class_is_zombified(mp->type))
      record_spell_set(mp, spell_sets);
    exper += exper_value(mp);
    mac += mp->ac;
    mev += mp->ev;
    set_min_max(mp->speed, speed_min, speed_max);
    set_min_max(mp->hit_points, hp_min, hp_max);

    // Destroy the monster.
    mp->reset();
    you.unique_creatures[spec_type] = false;

    rebind_mspec(&target, mname, &spec);

    index = mi_create_monster(spec);
    if (index == -1) {
      printf("Unexpected failure generating monster for %s\n",
             target.c_str());
      return 1;
    }
  }
  exper /= ntrials;
  mac /= ntrials;
  mev /= ntrials;

  monster &mon(menv[index]);

  const std::string symbol(monster_symbol(mon));

  const bool generated =
    mons_class_is_zombified(mon.type)
    || mon.type == MONS_HELL_BEAST || mon.type == MONS_PANDEMONIUM_LORD
    || mon.type == MONS_UGLY_THING || mon.type == MONS_DANCING_WEAPON;

  const bool shapeshifter =
      mon.is_shapeshifter()
      || spec_type == MONS_SHAPESHIFTER
      || spec_type == MONS_GLOWING_SHAPESHIFTER;

  const monsterentry *me =
      shapeshifter ? get_monster_data(spec_type) : mon.find_monsterentry();

  if (me)
  {
    std::string monsterflags;
    std::string monsterresistances;
    std::string monstervulnerabilities;
    std::string monsterattacks;

    lowercase(target);

    const bool changing_name =
      mon.has_hydra_multi_attack() || mon.type == MONS_PANDEMONIUM_LORD
        || mons_is_mimic(mon.type) || shapeshifter
        || mon.type == MONS_DANCING_WEAPON;

    printf("%s (%s)",
           changing_name ? me->name : mon.name(DESC_PLAIN, true).c_str(),
           symbol.c_str());

    if (mons_class_flag(mon.type, M_UNFINISHED))
        printf(" | %s", colour(LIGHTRED, "UNFINISHED").c_str());

    printf(" | Speed: %s",
           monster_speed(mon, me, speed_min, speed_max).c_str());

    const int hd = mon.hit_dice;
    printf(" | HD: %d", hd);

    printf(" | Health: ");
    const int hplow = hp_min;
    const int hphigh = hp_max;
    if (hplow < hphigh)
        printf("%i-%i", hplow, hphigh);
    else
        printf("%i", hplow);

    const int ac = generated? mac : me->AC;
    const int ev = generated? mev : me->ev;
    printf(" | AC/EV: %i/%i", ac, ev);

    mon.wield_melee_weapon();
    for (int x = 0; x < 4; x++)
    {
      mon_attack_def orig_attk(me->attack[x]);
      mon_attack_def attk = mons_attack_spec(&mon, x);
      if (attk.type)
      {
        if (monsterattacks.empty())
          monsterattacks = " | Damage: ";
        else
          monsterattacks += ", ";
        monsterattacks += to_string((short int) attk.damage);

        if (attk.type == AT_CONSTRICT)
            monsterattacks += colour(GREEN, "(constrict)");

        const attack_flavour flavour(
            orig_attk.flavour == AF_KLOWN ? AF_KLOWN
#if TAG_MAJOR_VERSION == 32
          : orig_attk.flavour == AF_SUBTRACTOR ? AF_SUBTRACTOR
#endif
          : attk.flavour);

        switch (flavour)
        {
        case AF_REACH:
          monsterattacks += "(reach)";
          break;
        case AF_ACID:
          monsterattacks += colour(YELLOW,
                                   damage_flavour("acid", "7d3"));
          break;
        case AF_BLINK:
          monsterattacks += colour(MAGENTA, "(blink)");
          break;
        case AF_COLD:
          monsterattacks +=
            colour(LIGHTBLUE, damage_flavour("cold", mon.hit_dice,
                                             3 * mon.hit_dice - 1));
          break;
        case AF_CONFUSE:
          monsterattacks += colour(LIGHTMAGENTA,"(confuse)");
          break;
        case AF_DISEASE:
          monsterattacks += colour(BROWN,"(disease)");
          break;
        case AF_DRAIN_DEX:
          monsterattacks += colour(RED,"(drain dexterity)");
          break;
        case AF_DRAIN_STR:
          monsterattacks += colour(RED,"(drain strength)");
          break;
        case AF_DRAIN_XP:
          monsterattacks += colour(LIGHTMAGENTA, "(drain)");
          break;
        case AF_CHAOS:
          monsterattacks += colour(LIGHTGREEN, "(chaos)");
          break;
        case AF_ELEC:
          monsterattacks +=
            colour(LIGHTCYAN,
                   damage_flavour("elec", mon.hit_dice,
                                  mon.hit_dice +
                                  std::max(mon.hit_dice / 2 - 1, 0)));
          break;
        case AF_FIRE:
          monsterattacks +=
            colour(LIGHTRED, damage_flavour("fire", mon.hit_dice,
                                            mon.hit_dice * 2 - 1));
          break;
        case AF_NAPALM:
          monsterattacks += colour(LIGHTRED, "(napalm)");
          break;
        case AF_HUNGER:
          monsterattacks += colour(BLUE, "(hunger)");
          break;
        case AF_MUTATE:
          monsterattacks += colour(LIGHTGREEN, "(mutation)");
          break;
        case AF_PARALYSE:
          monsterattacks += colour(LIGHTRED, "(paralyse)");
          break;
        case AF_POISON:
          monsterattacks += colour(YELLOW,"(poison)");
          break;
        case AF_POISON_NASTY:
          monsterattacks += colour(YELLOW,"(nasty poison)");
          break;
        case AF_POISON_MEDIUM:
          monsterattacks += colour(LIGHTRED,"(medium poison)");
          break;
        case AF_POISON_STRONG:
          monsterattacks += colour(LIGHTRED,"(strong poison)");
          break;
        case AF_POISON_STR:
          monsterattacks += colour(LIGHTRED,"(poison, drain str)");
          break;
        case AF_POISON_INT:
          monsterattacks += colour(LIGHTRED,"(poison, drain int)");
          break;
        case AF_POISON_DEX:
          monsterattacks += colour(LIGHTRED,"(poison, drain dex)");
          break;
        case AF_POISON_STAT:
          monsterattacks += colour(LIGHTRED,"(poison, drain stat)");
          break;
        case AF_ROT:
          monsterattacks += colour(LIGHTRED,"(rot)");
          break;
        case AF_VAMPIRIC:
          monsterattacks += colour(RED,"(vampiric)");
          break;
        case AF_KLOWN:
          monsterattacks += colour(LIGHTBLUE,"(klown)");
          break;
#if TAG_MAJOR_VERSION == 32
        case AF_SUBTRACTOR:
          monsterattacks += colour(DARKGREY, "(subtractor)");
          break;
#endif
        case AF_DISTORT:
          monsterattacks += colour(LIGHTBLUE,"(distort)");
          break;
        case AF_RAGE:
          monsterattacks += colour(RED,"(rage)");
          break;
        case AF_HOLY:
          monsterattacks += colour(YELLOW,"(holy)");
          break;
        case AF_PAIN:
          monsterattacks += colour(RED,"(pain)");
          break;
        case AF_ANTIMAGIC:
          monsterattacks += colour(LIGHTBLUE,"(antimagic)");
          break;
        case AF_DRAIN_INT:
          monsterattacks += colour(BLUE, "(drain int)");
          break;
        case AF_DRAIN_STAT:
          monsterattacks += colour(BLUE, "(drain stat)");
          break;
        case AF_STEAL:
          monsterattacks += colour(CYAN, "(steal)");
          break;
        case AF_STEAL_FOOD:
          monsterattacks += colour(CYAN, "(steal food)");
          break;
        case AF_CRUSH:
          monsterattacks += "(crush)";
          break;
        case AF_ENSNARE:
          monsterattacks += colour(WHITE, "(ensnare)");
          break;
        case AF_PLAIN:
          break;
// let the compiler issue warnings for us
//      default:
//        monsterattacks += "(???)";
//        break;
        }

        if (mon.has_hydra_multi_attack())
          monsterattacks += " per head";
      }
      if (mon.has_hydra_multi_attack())
        break;
    }

    printf("%s", monsterattacks.c_str());

    switch (me->holiness)
    {
    case MH_HOLY:
      mons_flag(monsterflags, colour(YELLOW, "holy"));
      break;
    case MH_UNDEAD:
      mons_flag(monsterflags, colour(BROWN, "undead"));
      break;
    case MH_DEMONIC:
      mons_flag(monsterflags, colour(RED, "demonic"));
      break;
    case MH_NONLIVING:
      mons_flag(monsterflags, colour(LIGHTCYAN, "non-living"));
      break;
    case MH_PLANT:
      mons_flag(monsterflags, colour(GREEN, "plant"));
      break;
    case MH_NATURAL:
    default:
      break;
    }

    mons_check_flag(me->habitat == HT_AMPHIBIOUS,
                    monsterflags, "amphibious");

    mons_check_flag(mon.is_evil(), monsterflags, "evil");
    mons_check_flag((me->bitfields & M_SPELLCASTER)
                    && (me->bitfields & M_ACTUAL_SPELLS),
                    monsterflags, "spellcaster");
    mons_check_flag(me->bitfields & M_COLD_BLOOD, monsterflags, "cold-blooded");
    mons_check_flag(me->bitfields & M_SENSE_INVIS, monsterflags,
                    "sense invisible");
    mons_check_flag(me->bitfields & M_SEE_INVIS, monsterflags, "see invisible");
    mons_check_flag(me->fly == FL_LEVITATE, monsterflags, "lev");
    mons_check_flag(me->fly == FL_FLY, monsterflags, "fly");
    mons_check_flag(monster_descriptor(mon.type, MDSC_REGENERATES),
                    monsterflags, "regen");
    mons_check_flag(me->bitfields & M_DEFLECT_MISSILES, monsterflags, "DMsl");
    mons_check_flag(me->bitfields & M_WEB_SENSE, monsterflags, "web sense");

    const std::string spell_abilities =
      mons_spells_abilities(&mon, shapeshifter, spell_sets);

    mons_check_flag(!spell_abilities.empty()
                    && !mon.is_priest() && !mon.is_actual_spellcaster()
                    && !mons_class_flag(mon.type, M_SPELL_NO_SILENT),
                    monsterflags, "!sil");

    mons_check_flag(vault_monster, monsterflags, colour(BROWN, "vault"));

    printf("%s", monsterflags.c_str());

    if (me->resist_magic == 5000)
    {
      if (monsterresistances.empty())
        monsterresistances = " | Res: ";
      else
        monsterresistances += ", ";
      monsterresistances += colour(LIGHTMAGENTA, "magic(immune)");
    }
    else if (me->resist_magic < 0)
    {
      if (monsterresistances.empty())
        monsterresistances = " | Res: ";
      else
        monsterresistances += ", ";
      monsterresistances += colour(MAGENTA, std::string() + "magic("
                                   + to_string((short int) mon.hit_dice * me->resist_magic * 4 / 3 * -1)
                                   + ")");
    }
    else if (me->resist_magic > 0)
    {
      if (monsterresistances.empty())
        monsterresistances = " | Res: ";
      else
        monsterresistances += ", ";
      monsterresistances += colour(MAGENTA, std::string("magic(")
                                   + to_string((short int) me->resist_magic)
                                   + ")");
    }

    const mon_resist_def res(
      shapeshifter? me->resists : get_mons_resists(&mon));
#define res(c,x)                                \
    do                                          \
    {                                           \
      record_resist(c,#x,                       \
                    monsterresistances,         \
                    monstervulnerabilities,     \
                    res.x);                     \
    } while (false)                             \

#define res2(c,x,y)                                \
    do                                          \
    {                                           \
      record_resist(c,#x,                       \
                    monsterresistances,         \
                    monstervulnerabilities,     \
                    y);                         \
    } while (false)                             \


    res(RED,hellfire);
    if (me->resists.hellfire <= 0)
      res(RED,fire);
    res(BLUE,cold);
    res(CYAN,elec);
    res(GREEN,poison);
    res(BROWN,acid);
    res(0,steam);
    res(0,asphyx);

    res2(LIGHTBLUE,    drown, mon.res_water_drowning());
    res2(LIGHTRED,     rot,   mon.res_rotting());
    res2(LIGHTMAGENTA, neg,   mon.res_negative_energy());
    res2(YELLOW,       holy,  mon.res_holy_energy(&you));
    res2(LIGHTMAGENTA, torm,  mon.res_torment());
    res2(LIGHTBLUE,    wind,  mon.res_wind());

    printf("%s", monsterresistances.c_str());
    printf("%s", monstervulnerabilities.c_str());

    if (me->weight != 0 && me->corpse_thingy != CE_NOCORPSE && me->corpse_thingy != CE_CLEAN)
    {
      printf(" | Chunks: ");
      switch (me->corpse_thingy)
      {
      case CE_CONTAMINATED:
        printf("%s", colour(BROWN,"contaminated").c_str());
        break;
      case CE_POISONOUS:
        printf("%s", colour(LIGHTGREEN,"poisonous").c_str());
        break;
      case CE_POISON_CONTAM:
        printf("%s+%s", colour(LIGHTGREEN,"poison").c_str(),
                        colour(BROWN,"contam").c_str());
        break;
      case CE_ROT:
        printf("%s", colour(LIGHTRED,"rot").c_str());
        break;
      case CE_MUTAGEN_RANDOM:
        printf("%s", colour(MAGENTA, "mutagenic").c_str());
        break;
      default:
        printf("???");
        break;
      }
    }

    printf(" | XP: %ld", exper);

    if (!spell_abilities.empty())
      printf(" | Sp: %s", spell_abilities.c_str());

    printf(".\n");

    return 0;
  }
  return 1;
}

template <class T> inline std::string to_string (const T& t)
{
  std::stringstream ss;
  ss << t;
  return ss.str();
}

//////////////////////////////////////////////////////////////////////////
// acr.cc stuff

CLua clua(true);
CLua dlua(false);      // Lua interpreter for the dungeon builder.
crawl_environment env; // Requires dlua.
player you;
game_state crawl_state;

FILE *yyin;
int yylineno;

std::string init_file_error;    // externed in newgame.cc

char info[ INFO_SIZE ];         // messaging queue extern'd everywhere {dlb}

int stealth;                    // externed in view.cc
bool apply_berserk_penalty;     // externed in evoke.cc

void process_command(command_type) {
}

int yyparse() {
  return 0;
}

void world_reacts() {
}
