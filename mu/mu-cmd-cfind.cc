/*
** Copyright (C) 2022-2023 Dirk-Jan C. Binnema <djcb@djcbsoftware.nl>
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of the GNU General Public License as published by the
** Free Software Foundation; either version 3, or (at your option) any
** later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation,
** Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
**
*/

#include "config.h"
#include "mu-cmd.hh"

#include <cstdint>
#include <string>
#include <functional>
#include <unordered_map>

#include <utils/mu-utils.hh>
#include <utils/mu-regex.hh>
#include <utils/mu-option.hh>

using namespace Mu;

enum struct ItemType { Header, Footer, Normal };
using OutputFunc = std::function<void(ItemType itype, Option<const Contact&>, const Options&)>;
using OptContact = Option<const Contact&>;
using Format = Options::Cfind::Format;

// simplistic guess of first & last names, for setting
// some initial value.
static std::pair<std::string, std::string>
guess_first_last_name(const std::string& name)
{
	if (name.empty())
		return {};

	const auto lastspc = name.find_last_of(' ');
	if (lastspc == name.npos)
		return { name, "" }; // no last name
	else
		return { name.substr(0, lastspc), name.substr(lastspc + 1)};
}


// candidate nick and a _count_ for that given nick, to uniquify them.
static std::unordered_map<std::string, size_t> nicks;
static std::string
guess_nick(const Contact& contact)
{
	auto cleanup = [](const std::string& str) {
		std::string clean;
		for (auto& c: str) // XXX: support non-ascii
			if (!::ispunct(c) && !::isspace(c))
				clean += c;
		return clean;
	};

	auto nick = cleanup(std::invoke([&]()->std::string {

		// no name? use the user part from the addr
		if (contact.name.empty()) {
			const auto pos{contact.email.find('@')};
			if (pos == std::string::npos)
				return contact.email; // no '@'
			else
				return contact.email.substr(0, pos);
		}

		const auto names{guess_first_last_name(contact.name)};
		/* if there's no last name, use first name as the nick */
		if (names.second.empty())
			return names.first;

		char initial[7] = {};
		if (g_unichar_to_utf8(g_utf8_get_char(names.second.c_str()), initial) == 0) {
			    /* couldn't we get an initial for the last name?
			     * just use the first name*/
			    return names.first;
		} else // prepend the initial
			return names.first + initial;
	}));

	// uniquify.
	if (auto it = nicks.find(nick); it == nicks.cend())
		nicks.emplace(nick, 0);
	else {
		++it->second;
		nick = format("%s%zu", nick.c_str(), ++it->second);
	}

	return nick;
}


static void
output_plain(ItemType itype, OptContact contact, const Options& opts)
{
	if (!contact)
		return;

	const auto col1{opts.nocolor ? "" : MU_COLOR_MAGENTA};
	const auto col2{opts.nocolor ? "" : MU_COLOR_GREEN};
	const auto coldef{opts.nocolor ? "" : MU_COLOR_DEFAULT};

	mu_print_encoded("{}{}{}{}{}{}{}\n",
			 col1, contact->name, coldef,
			 contact->name.empty() ? "" : " ",
			 col2, contact->email, coldef);
}

static void
output_mutt_alias(ItemType itype, OptContact contact, const Options& opts)
{
	if (!contact)
		return;

	const auto nick{guess_nick(*contact)};
	mu_print_encoded("alias {} {} <{}>\n", nick, contact->name, contact->email);

}

static void
output_mutt_address_book(ItemType itype, OptContact contact, const Options& opts)
{
	if (itype == ItemType::Header)
		mu_print ("Matching addresses in the mu database:\n");

	if (contact)
		mu_print_encoded("{}\t{}\t\n", contact->email, contact->name);
}

static void
output_wanderlust(ItemType itype, OptContact contact, const Options& opts)
{
	if (!contact || contact->name.empty())
		return;

	auto nick=guess_nick(*contact);

	mu_print_encoded("{} \"{}\" \"{}\"\n", contact->email, nick, contact->name);

}

static void
output_org_contact(ItemType itype, OptContact contact, const Options& opts)
{
	if (!contact || contact->name.empty())
		return;

	mu_print_encoded("* {}\n:PROPERTIES:\n:EMAIL: {}\n:END:\n\n",
			 contact->name, contact->email);
}

static void
output_bbdb(ItemType itype, OptContact contact, const Options& opts)
{
	if (itype == ItemType::Header)
		mu_println (";; -*-coding: utf-8-emacs;-*-\n"
			    ";;; file-version: 6");
	if (!contact)
		return;

	const auto names{guess_first_last_name(contact->name)};
	const auto now{time_to_string("%Y-%m-%d", ::time(NULL))};
	const auto timestamp{time_to_string("%Y-%m-%d", contact->message_date)};

	mu_println("[\"{}\" \"{}\" nil nil nil nil (\"{}\") "
		"((creation-date . \"{}\") (time-stamp . \"{}\")) nil]",
		names.first, names.second, contact->email, now, timestamp);
}

static void
output_csv(ItemType itype, OptContact contact, const Options& opts)
{
	if (!contact)
		return;

	mu_print_encoded("{},{}\n",
			 contact->name.empty() ? "" : Mu::quote(contact->name),
			 Mu::quote(contact->email));
}

static void
output_json(ItemType itype, OptContact contact, const Options& opts)
{
	if (itype == ItemType::Header)
		mu_println("[");
	if (contact) {
		mu_print("{}", itype == ItemType::Header ? "" : ",\n");
		mu_println ("  {{");

		const std::string name = contact->name.empty() ? "null" : Mu::quote(contact->name);
		mu_print_encoded(
			"    \"email\"         : \"{}\",\n"
			"    \"name\"          : {},\n"
			"    \"display\"       : {},\n"
			"    \"last-seen\"     : {},\n"
			"    \"last-seen-iso\" : \"{}\",\n"
			"    \"personal\"      : {},\n"
			"    \"frequency\"     : {}\n",
			contact->email,
			name,
			Mu::quote(contact->display_name()),
			contact->message_date,
			time_to_string("%FT%TZ", contact->message_date, true/*utc*/),
			contact->personal ? "true" : "false",
			contact->frequency);
		mu_print("  }}");
	}

	if (itype == ItemType::Footer)
		mu_println("\n]");
}

static OutputFunc
find_output_func(Format format)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
	switch(format) {
	case Format::Plain:
		return output_plain;
	case Format::MuttAlias:
		return output_mutt_alias;
	case Format::MuttAddressBook:
		return output_mutt_address_book;
	case Format::Wanderlust:
		return output_wanderlust;
	case Format::OrgContact:
		return output_org_contact;
	case Format::Bbdb:
		return output_bbdb;
	case Format::Csv:
		return output_csv;
	case Format::Json:
		return output_json;
	default:
		mu_warning("unsupported format");
		return {};
	}
#pragma GCC diagnostic pop
}


Result<void>
Mu::mu_cmd_cfind(const Mu::Store& store, const Mu::Options& opts)
{
	size_t num{};
	OutputFunc output = find_output_func(opts.cfind.format);
	if (!output)
		return Err(Error::Code::Internal,
			   "missing output function");

	// get the pattern regex, if any.
	Regex rx{};
	if (!opts.cfind.rx_pattern.empty()) {
		if (auto&& res =  Regex::make(opts.cfind.rx_pattern,
					      static_cast<GRegexCompileFlags>
					      (G_REGEX_OPTIMIZE|G_REGEX_CASELESS)); !res)
			return Err(std::move(res.error()));
		else
			rx = res.value();
	}

	nicks.clear();
	store.contacts_cache().for_each([&](const Contact& contact)->bool {

		if (opts.cfind.maxnum && num > *opts.cfind.maxnum)
			return false; /* stop the loop */

		if (!contact.has_valid_email())
			return true; /* next */

		// filter for maxnum, personal  & "after"
		if ((opts.cfind.personal && !contact.personal) ||
		    (opts.cfind.after.value_or(0) > contact.message_date))
			return true; /* next */

		// filter for regex, if any.
		if (rx) {
			if (!rx.matches(contact.name) && !rx.matches(contact.email))
				return true; /* next */
		}

		/* seems we have a match! display it. */
		const auto itype{num == 0 ? ItemType::Header : ItemType::Normal};
		output(itype, contact, opts);
		++num;
		return true;
	});

	if (num == 0)
		return Err(Error::Code::NoMatches, "no matching contacts found");

	output(ItemType::Footer, Nothing, opts);
	return Ok();
}
