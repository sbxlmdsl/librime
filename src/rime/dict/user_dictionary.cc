//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-10-30 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utf8.h>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/scope_exit.hpp>
#include <rime/common.h>
#include <rime/language.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/algo/dynamics.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/db.h>
#include <rime/dict/table.h>
#include <rime/dict/user_dictionary.h>
#include <rime/gear/unity_table_encoder.h>

namespace rime {

	struct DfsState {
		size_t depth_limit;
		TickCount present_tick;
		Code code;
		vector<double> credibility;
		an<UserDictEntryCollector> collector;
		an<DbAccessor> accessor;
		string key;
		string value;

		bool IsExactMatch(const string& prefix) {
			return boost::starts_with(key, prefix + '\t');
		}
		bool IsPrefixMatch(const string& prefix) {
			return boost::starts_with(key, prefix);
		}
		void RecruitEntry(size_t pos);
		bool NextEntry() {
			if (!accessor->GetNextRecord(&key, &value)) {
				key.clear();
				value.clear();
				return false;  // reached the end
			}
			return true;
		}
		bool ForwardScan(const string& prefix) {
			if (!accessor->Jump(prefix)) {
				return false;
			}
			return NextEntry();
		}
		bool Backdate(const string& prefix) {
			DLOG(INFO) << "backdate; prefix: " << prefix;
			if (!accessor->Reset()) {
				LOG(WARNING) << "backdating failed for '" << prefix << "'.";
				return false;
			}
			return NextEntry();
		}
	};

	void DfsState::RecruitEntry(size_t pos) {
		auto e = UserDictionary::CreateDictEntry(key, value, present_tick,
			credibility.back());
		if (e) {
			e->code = code;
			DLOG(INFO) << "add entry at pos " << pos;
			(*collector)[pos].push_back(e);
		}
	}

	// UserDictEntryIterator members

	void UserDictEntryIterator::Add(const an<DictEntry>& entry) {
		if (!entries_) {
			entries_ = New<DictEntryList>();
		}
		entries_->push_back(entry);
	}

	void UserDictEntryIterator::SortRange(size_t start, size_t count) {
		if (entries_)
			entries_->SortRange(start, count);
	}

	bool UserDictEntryIterator::Release(DictEntryList* receiver) {
		if (!entries_)
			return false;
		if (receiver)
			entries_->swap(*receiver);
		entries_.reset();
		index_ = 0;
		return true;
	}

	bool UserDictEntryIterator::SetIndex(size_t index) {
		if (index < 0 || index + 1 > entries_->size())
			return false;
		index_ = 0;
		return true;
	}

	void UserDictEntryIterator::AddFilter(DictEntryFilter filter) {
		DictEntryFilterBinder::AddFilter(filter);
		// the introduced filter could invalidate the current or even all the
		// remaining entries
		while (!exhausted() && !filter_(Peek())) {
			FindNextEntry();
		}
	}

	an<DictEntry> UserDictEntryIterator::Peek() {
		if (exhausted()) {
			return nullptr;
		}
		return (*entries_)[index_];
	}

	bool UserDictEntryIterator::FindNextEntry() {
		if (exhausted()) {
			return false;
		}
		++index_;
		return !exhausted();
	}

	bool UserDictEntryIterator::Next() {
		if (!FindNextEntry()) {
			return false;
		}
		while (filter_ && !filter_(Peek())) {
			if (!FindNextEntry()) {
				return false;
			}
		}
		return true;
	}

	// UserDictionary members

	UserDictionary::UserDictionary(const string& name, an<Db> db, const string& schema)
		: name_(name), db_(db), schema_(schema) {
	}

	UserDictionary::UserDictionary(const string& name, an<Db> db, const string& schema, const int& delete_threshold, 
		const bool& enable_filtering, const bool& forced_selection, const bool& single_selection, const bool& strong_mode, const bool& lower_case)
		: name_(name), db_(db), schema_(schema), delete_threshold_(delete_threshold), enable_filtering_(enable_filtering), 
		forced_selection_(forced_selection), single_selection_(single_selection), strong_mode_(strong_mode), lower_case_(lower_case) {
	}

	UserDictionary::~UserDictionary() {
		if (loaded()) {
			CommitPendingTransaction();
		}
	}

	void UserDictionary::Attach(const an<Table>& table,
		const an<Prism>& prism) {
		table_ = table;
		prism_ = prism;
	}

	bool UserDictionary::Load() {
		if (!db_)
			return false;
		if (!db_->loaded() && !db_->Open()) {
			// try to recover managed db in available work thread
			Deployer& deployer(Service::instance().deployer());
			auto task = DeploymentTask::Require("userdb_recovery_task");
			if (task && Is<Recoverable>(db_) && !deployer.IsWorking()) {
				deployer.ScheduleTask(an<DeploymentTask>(task->Create(db_)));
				deployer.StartWork();
			}
			return false;
		}
		if (!FetchTickCount() && !Initialize())
			return false;
		return true;
	}

	bool UserDictionary::loaded() const {
		return db_ && !db_->disabled() && db_->loaded();
	}

	bool UserDictionary::readonly() const {
		return db_ && db_->readonly();
	}

	// this is a one-pass scan for the user db which supports sequential access
	// in alphabetical order (of syllables).
	// each call to DfsLookup() searches for matching phrases at a given
	// start position: current_pos.
	// there may be multiple edges that start at current_pos, and ends at different
	// positions after current_pos. on each edge, there can be multiple syllables
	// the spelling on the edge maps to.
	// in order to enable forward scaning and to avoid backdating, our strategy is:
	// sort all those syllables from edges that starts at current_pos, so that
	// the syllables are in the same alphabetical order as the user db's.
	// this having been done by transposing the syllable graph into
	// SyllableGraph::index.
	// however, in the case of 'shsh' which could be the abbreviation of either
	// 'sh(a) sh(i)' or 'sh(a) s(hi) h(ou)',
	// we now have to give up the latter path in order to avoid backdating.

	// update: 2013-06-25
	// to fix the following issue, we have to reintroduce backdating in db scan:
	// given aaa=A, b=B, ab=C, derive/^(aa)a$/$1/,
	// the input 'aaab' can be either aaa'b=AB or aa'ab=AC.
	// note that backdating works only for normal or fuzzy spellings, but not for
	// abbreviations such as 'shsh' in the previous example.

	void UserDictionary::DfsLookup(const SyllableGraph& syll_graph,
		size_t current_pos,
		const string& current_prefix,
		DfsState* state) {
		auto index = syll_graph.indices.find(current_pos);
		if (index == syll_graph.indices.end()) {
			return;
		}
		DLOG(INFO) << "dfs lookup starts from " << current_pos;
		string prefix;
		for (const auto& spelling : index->second) {
			DLOG(INFO) << "prefix: '" << current_prefix << "'"
				<< ", syll_id: " << spelling.first
				<< ", num_spellings: " << spelling.second.size();
			state->code.push_back(spelling.first);
			BOOST_SCOPE_EXIT((&state)) {
				state->code.pop_back();
			}
			BOOST_SCOPE_EXIT_END
				if (!TranslateCodeToString(state->code, &prefix))
					continue;
			for (size_t i = 0; i < spelling.second.size(); ++i) {
				auto props = spelling.second[i];
				if (i > 0 && props->type >= kAbbreviation)
					continue;
				state->credibility.push_back(
					state->credibility.back() + props->credibility);
				BOOST_SCOPE_EXIT((&state)) {
					state->credibility.pop_back();
				}
				BOOST_SCOPE_EXIT_END
					size_t end_pos = props->end_pos;
				DLOG(INFO) << "edge: [" << current_pos << ", " << end_pos << ")";
				if (prefix != state->key) {  // 'a b c |d ' > 'a b c \tabracadabra'
					DLOG(INFO) << "forward scanning for '" << prefix << "'.";
					if (!state->ForwardScan(prefix))  // reached the end of db
						continue;
				}
				while (state->IsExactMatch(prefix)) {  // 'b |e ' vs. 'b e \tBe'
					DLOG(INFO) << "match found for '" << prefix << "'.";
					state->RecruitEntry(end_pos);
					if (!state->NextEntry())  // reached the end of db
						break;
				}
				// the caller can limit the number of syllables to look up
				if ((!state->depth_limit || state->code.size() < state->depth_limit) &&
					state->IsPrefixMatch(prefix)) {  // 'b |e ' vs. 'b e f \tBefore'
					DfsLookup(syll_graph, end_pos, prefix, state);
				}
			}
			if (!state->IsPrefixMatch(current_prefix))  // 'b |' vs. 'g o \tGo'
				return;
			// 'b |e ' vs. 'b y \tBy'
		}
	}

	an<UserDictEntryCollector>
		UserDictionary::Lookup(const SyllableGraph& syll_graph,
			size_t start_pos,
			size_t depth_limit,
			double initial_credibility) {
		if (!table_ || !prism_ || !loaded() ||
			start_pos >= syll_graph.interpreted_length)
			return nullptr;
		DfsState state;
		state.depth_limit = depth_limit;
		FetchTickCount();
		state.present_tick = tick_ + 1;
		state.credibility.push_back(initial_credibility);
		state.collector = New<UserDictEntryCollector>();
		state.accessor = db_->Query("");
		state.accessor->Jump(" ");  // skip metadata
		string prefix;
		DfsLookup(syll_graph, start_pos, prefix, &state);
		if (state.collector->empty())
			return nullptr;
		// sort each group of homophones by weight
		for (auto& v : *state.collector) {
			v.second.Sort();
		}
		return state.collector;
	}

	size_t UserDictionary::LookupWords(UserDictEntryIterator* result,
		const string& input,
		bool predictive,
		size_t limit,
		string* resume_key) {
		TickCount present_tick = tick_ + 1;
		size_t len = input.length();
		size_t start = result->size();
		size_t count = 0;
		size_t exact_match_count = 0;
		const string kEnd = "\xff";
		string key;
		string value;
		string full_code;
		an<DbAccessor> accessor;
		static char words[7][256];

		const bool prefixed = boost::starts_with(input, "\x7f""enc\x1f");

		if (boost::regex_match(name_, boost::regex("^sbjm|sbdp|sb[fk]mk|sb[fk][jx]$"))) {
			if (len < 3) {
				accessor = db_->Query(input);
			}
			else if (prefixed) {
				if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && strong_mode_ || boost::regex_match(name_, boost::regex("^sb[fk]x$"))
						&& len >= 8 && string("qwrtsdfgzxcvbyphjklnm").find(input[7]) != string::npos) {
					if (len == 8)
						return 0;
					//if (len == 9 && string("aeuio").find(input[8]) != string::npos)
					//	return 0;
					//if (name_ != "sbjm" && len == 9 && string("_qwrtsdfgzxcvbyphjklnm2378901456").find(input[8]) != string::npos)
					//	return 0;
				}
				else if (len == 9 && boost::regex_match(name_, boost::regex("^sb[fk]x$"))
					&& string("aeuio").find(input[8]) != string::npos && string("qwrtsdfgzxcvbyphjklnm").find(input[9]) != string::npos)
					return 0;
				accessor = db_->Query(input.substr(0, 8));
			}
			else {
				if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && strong_mode_ || boost::regex_match(name_, boost::regex("^sb[fk]x$"))
						&& len >= 3 && string("qwrtsdfgzxcvbyphjklnm").find(input[2]) != string::npos) {
					if (len == 3)
						return 0;
					//if (len == 4 && string("aeuio").find(input[3]) != string::npos)
					//	return 0;
					//if (name_ != "sbjm" && len == 4 && string("_qwrtsdfgzxcvbyphjklnm2378901456").find(input[3]) != string::npos)
					//	return 0;
				}
				else if (len == 4 && boost::regex_match(name_, boost::regex("^sb[fk]x$")) 
					&& string("aeuio").find(input[2]) != string::npos && string("qwrtsdfgzxcvbyphjklnm").find(input[3]) != string::npos)
					return 0;
				accessor = db_->Query(input.substr(0, 3));
			}
		}
		else if (boost::regex_match(name_, boost::regex("^sbxh|sbzr|sbjk|sb[fk]m|sb[fk]ms|sb[fk]s$"))) {
			if (len < 4) {
				accessor = db_->Query(input);
			}
			else if (prefixed) {
				accessor = db_->Query(input.substr(0, 9));
			}
			else {
				accessor = db_->Query(input.substr(0, 4));
			}
		}
		else {
			accessor = db_->Query(input);
		}

		if (!accessor || accessor->exhausted()) {
			if (resume_key)
				*resume_key = kEnd;
			return 0;
		}
		if (resume_key && !resume_key->empty()) {
			if (!accessor->Jump(*resume_key) ||
				!accessor->GetNextRecord(&key, &value)) {
				*resume_key = kEnd;
				return 0;
			}
			DLOG(INFO) << "resume lookup after: " << key;
		}

		string last_key(key);
		an<DictEntry> e_holder = nullptr;
		while (accessor->GetNextRecord(&key, &value)) {
			DLOG(INFO) << "key : " << key << ", value: " << value;
			bool is_exact_match = (len < key.length() && key[len] == ' ');
			//skip multi-char words when len is 3 or 8
/*			if (boost::regex_match(name_, boost::regex("^sbdp$")) && (len == 3 || len == 8) && is_exact_match) {
				if (prefixed && string("qwrtsdfgzxcvbyphjklnm").find(key.substr(10, 1)) != string::npos
					&& boost::regex_match(key.substr(5, 3), boost::regex("^[qwrtsdfgzxcvbyphjklnm]{3}$")))
					continue;
				if (!prefixed && string("qwrtsdfgzxcvbyphjklnm").find(key.substr(5, 1)) != string::npos
					&& boost::regex_match(key.substr(0, 3), boost::regex("^[qwrtsdfgzxcvbyphjklnm]{3}$")))
					continue;
			} */
			if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && (len == 3 || len == 8) && is_exact_match) {
				if (prefixed && string("QWRTSDFGZXCVBYPHJKLNM").find(key.substr(13, 1)) != string::npos
					&& boost::regex_match(key.substr(5, 3), boost::regex("^[qwrtsdfgzxcvbyphjklnm]{3}$")))
					continue;
				if (!prefixed && string("QWRTSDFGZXCVBYPHJKLNM").find(key.substr(8, 1)) != string::npos
					&& boost::regex_match(key.substr(0, 3), boost::regex("^[qwrtsdfgzxcvbyphjklnm]{3}$")))
					continue;
			}
			if (!is_exact_match && prefixed && len > 8 && boost::regex_match(name_, boost::regex("^sbjm|sbdp|sb[fk]mk|sb[fk][jx]$"))) {
				string key_holder = key;
				if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && string("QWRTSDFGZXCVBYPHJKLNM").find(key[13]) != string::npos
					&& (string("QWRTSDFGZXCVBYPHJKLNM").find(input[8]) != string::npos || lower_case_ && string("qwrtsdfgzxcvbyphjklnm").find(input[8]) != string::npos))
					key_holder[10] = key[13];
				else if (boost::regex_match(name_, boost::regex("^sb[fk]x$")) && string("QWRTSDFGZXCVBYPHJKLNM").find(key[14]) != string::npos
					&& (string("QWRTSDFGZXCVBYPHJKLNM").find(input[8]) != string::npos))
					key_holder[10] = key[14];
				string input_holder = input;
				if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && lower_case_ && string("qwrtsdfgzxcvbyphjklnm").find(input[8]) != string::npos && string("qwrtsdfgzxcvbyphjklnm").find(input[7]) != string::npos) {
					input_holder[8] = toupper(input[8]);
				}
				else if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && string("23789").find(input[8]) != string::npos && strong_mode_ && string("qwrtsdfgzxcvbyphjklnm").find(input[7]) != string::npos) {
					map<char, char> m = { {'2','a'},{'3','e'},{'7','u'},{'8','i'},{'9','o'} };
					if (enable_filtering_ && string("QWRTSDFGZXCVBYPHJKLNM").find(key[13]) != string::npos)
						continue;
					input_holder[8] = m[input[8]];
				}
				string r1;
				if (len == 10 && boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && !single_selection_)
					r1 = input_holder.substr(8, 1);
				else if (len == 11 && boost::regex_match(name_, boost::regex("^sb[fk]x$")) && !single_selection_)
					r1 = input_holder.substr(8, 2);
				else
					r1 = input_holder.substr(8, len - 8);
				string r2;
				if (len == 10 && boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && !single_selection_)
					r2 = key_holder.substr(10, 1);
				else if (len == 11 && boost::regex_match(name_, boost::regex("^sb[fk]x$")) && !single_selection_)
					r2 = key_holder.substr(10, 2);
				else
					r2 = key_holder.substr(10, len - 8);
				if (r1 == r2) {
					is_exact_match = true;
				}
				else {
					continue;
				}
			}
			else if (!is_exact_match && len > 3 && boost::regex_match(name_, boost::regex("^sbjm|sbdp|sb[fk]mk|sb[fk][jx]$"))) {
				string key_holder = key;
				if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && string("QWRTSDFGZXCVBYPHJKLNM").find(key[8]) != string::npos
					&& (string("QWRTSDFGZXCVBYPHJKLNM").find(input[3]) != string::npos || lower_case_ && string("qwrtsdfgzxcvbyphjklnm").find(input[3]) != string::npos))
					key_holder[5] = key[8];
				else if (boost::regex_match(name_, boost::regex("^sb[fk]x$")) && string("QWRTSDFGZXCVBYPHJKLNM").find(key[9]) != string::npos
					&& (string("QWRTSDFGZXCVBYPHJKLNM").find(input[3]) != string::npos))
					key_holder[5] = key[9];
				string input_holder = input;
				if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && lower_case_ && string("qwrtsdfgzxcvbyphjklnm").find(input[3]) != string::npos && string("qwrtsdfgzxcvbyphjklnm").find(input[2]) != string::npos) {
					input_holder[3] = toupper(input[3]);
				}
				else if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && string("23789").find(input[3]) != string::npos && strong_mode_ && string("qwrtsdfgzxcvbyphjklnm").find(input[2]) != string::npos) {
					map<char, char> m = { {'2','a'},{'3','e'},{'7','u'},{'8','i'},{'9','o'} };
					if (enable_filtering_ && string("QWRTSDFGZXCVBYPHJKLNM").find(key[8]) != string::npos)
						continue;
					input_holder[3] = m[input[3]];
				}
				string r1;
				if (len == 5 && boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && !single_selection_)
					r1 = input_holder.substr(3, 1);
				else if (len == 6 && boost::regex_match(name_, boost::regex("^sb[fk]x$")) && !single_selection_)
					r1 = input_holder.substr(3, 2);
				else
					r1 = input_holder.substr(3, len - 3);
				string r2;
				if (len == 5 && boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && !single_selection_)
					r2 = key_holder.substr(5, 1);
				else if (len == 6 && boost::regex_match(name_, boost::regex("^sb[fk]x$")) && !single_selection_)
					r2 = key_holder.substr(5, 2);
				else
					r2 = key_holder.substr(5, len - 3);
				if (r1 == r2) {
					is_exact_match = true;
				}
				else {
					continue;
				}
			}
			else if (!is_exact_match && prefixed && len > 9 && boost::regex_match(name_, boost::regex("^sbxh|sbzr|sbjk|sb[fk]m|sb[fk]ms|sb[fk]s$"))) {
				string r1 = (len == 10 && boost::regex_match(name_, boost::regex("^sbxh|sbzr|sbjk|sb[fk]m|sb[fk]s$")) && !single_selection_) ? input.substr(9, 0) : input.substr(9, len - 9);
				string r2 = (len == 10 && boost::regex_match(name_, boost::regex("^sbxh|sbzr|sbjk|sb[fk]m|sb[fk]s$")) && !single_selection_) ? key.substr(11, 0) : key.substr(11, len - 9);
				if (r1 == r2) {
					is_exact_match = true;
				}
				else {
					continue;
				}
			}
			else if (!is_exact_match && len > 4 && boost::regex_match(name_, boost::regex("^sbxh|sbzr|sbjk|sb[fk]m|sb[fk]ms|sb[fk]s$"))) {
				string r1 = (len == 5 && boost::regex_match(name_, boost::regex("^sbxh|sbzr|sbjk|sb[fk]m|sb[fk]s$")) && !single_selection_) ? input.substr(4, 0) : input.substr(4, len - 4);
				string r2 = (len == 5 && boost::regex_match(name_, boost::regex("^sbxh|sbzr|sbjk|sb[fk]m|sb[fk]s$")) && !single_selection_) ? key.substr(6, 0) : key.substr(6, len - 4);
				if (r1 == r2) {
					is_exact_match = true;
				}
				else {
					continue;
				}
			}
			if (!is_exact_match && !predictive) {
				key = last_key;
				break;
			}
			last_key = key;
			auto e = CreateDictEntry(key, value, present_tick, 1.0, &full_code);
			if (!e)
				continue;
			e->custom_code = full_code;
			boost::trim_right(full_code);  // remove trailing space a user dict key has
			if (full_code.length() > len) {
				e->comment = "~" + full_code.substr(len);
				e->remaining_code_length = full_code.length() - len;
			}
			if (boost::regex_match(name_, boost::regex("^sbjm|sbdp|sb[fk]mk|sb[fk]j$")) && (len == 3 || (prefixed && len == 8))) {
				if (!e_holder) {
					e_holder = e;
				}
				else if (e_holder->weight < e->weight) {
					e_holder = e;
				}
				continue;
			}
			else if (boost::regex_match(name_, boost::regex("^sbjm|sbxh|sbzr|sbjk|sb[fk]m|sbdp|sb[fk]m[ks]|sb[fk][jsx]$")) && (len == 4 || (prefixed && len == 9))) {
				int l = len == 4 ? 3 : 8;
				if (boost::regex_match(name_, boost::regex("^sb[fk]x$")) && string("aeuio").find(input[l]) != string::npos	&& last_key[l + 3] != ' ')
					continue;
				if (e->text == string(words[0]))
					continue;
				else if (boost::regex_match(name_, boost::regex("^sbjm|sbdp|sb[fk]s|sbxh|sbzr|sbjk|sb[fk][mx]$")) && !single_selection_) {
					if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && enable_filtering_ && string("aeuio").find(input[l]) != string::npos
						&& 9 <= utf8::unchecked::distance(e->text.c_str(), e->text.c_str() + e->text.length()))
						continue;
					else if (prefixed && len == 9 && delete_threshold_ > 0) {
						if (!DeleteEntry(e))
							result->Add(e);
						else
							continue;
					}
					else
						result->Add(e);
				}
				else {
					if (!e_holder) {
						e_holder = e;
					}
					else if (e_holder->weight < e->weight) {
						e_holder = e;
					}
					continue;
				}
			}
			else if (boost::regex_match(name_, boost::regex("^sbjm|sbxh|sbzr|sbjk|sb[fk]m|sbdp|sb[fk]m[ks]|sb[fk][jsx]$")) && (len == 5 || (prefixed && len == 10))) {
				if (boost::regex_match(name_, boost::regex("^sb[fk]x$"))) {
					if (enable_filtering_ && 9 <= utf8::unchecked::distance(e->text.c_str(), e->text.c_str() + e->text.length()))
						continue;
					if (!single_selection_) {
						if (prefixed && len == 10 && delete_threshold_ > 0) {
							if (!DeleteEntry(e))
								result->Add(e);
							else
								continue;
						}
						else
							result->Add(e);
					}
					else {
						if (!e_holder) {
							e_holder = e;
						}
						else if (e_holder->weight < e->weight) {
							e_holder = e;
						}
						continue;
					}
				}
				else if (boost::regex_match(name_, boost::regex("^sbjm|sbdp|sb[fk]s|sbxh|sbzr|sbjk|sb[fk]m$")) && !single_selection_) {
					int i = 0;
					int j = (len == 5) ? 4 : 9;
					switch (input[j]) {
					case 'a':
						i = 2; break;
					case 'e':
						i = 3; break;
					case 'u':
						i = 4; break;
					case 'i':
						i = 5; break;
					case 'o':
						i = 6; break;
					}
					if (i == 0 || words[i] == string(""))
						return 0;
					if (e->text != string(words[i]))
						continue;
					else {
						result->Add(e);
						return 1;
					}
				}
				else {
					int i;
					for (i = 0; i < 2; i++) {
						if (e->text == string(words[i]))
							break;
					}
					if (i < 2)
						continue;

					if (!e_holder) {
						e_holder = e;
					}
					else if (e_holder->weight < e->weight) {
						e_holder = e;
					}
					continue;
				}
			}
			else if (boost::regex_match(name_, boost::regex("^sbjm|sbxh|sbzr|sbjk|sb[fk]m|sbdp|sb[fk]m[ks]|sb[fk][sx]$")) && (len == 6 || (prefixed && len == 11))) {
				if (boost::regex_match(name_, boost::regex("^sb[fk]x$"))) {
					if (enable_filtering_ && 9 <= utf8::unchecked::distance(e->text.c_str(), e->text.c_str() + e->text.length()))
						continue;
					if (!single_selection_) {
						int i = 0;
						int j = (len == 6) ? 5 : 10;
						switch (input[j]) {
						case 'a':
							i = 2; break;
						case 'e':
							i = 3; break;
						case 'u':
							i = 4; break;
						case 'i':
							i = 5; break;
						case 'o':
							i = 6; break;
						}
						if (i == 0 || words[i] == string(""))
							return 0;
						if (e->text != string(words[i]))
							continue;
						else {
							result->Add(e);
							return 1;
						}
					}
					else {
						int l = len == 6 ? 3 : 8;
						if (boost::regex_match(name_, boost::regex("^sb[fk]x$")) && string("aeuio").find(input[l]) != string::npos	&& last_key[l + 3] == ' ')
							continue;
						int i;
						for (i = 0; i < 2; i++) {
							if (e->text == string(words[i]))
								break;
						}
						if (i < 2)
							continue;

						if (!e_holder) {
							e_holder = e;
						}
						else if (e_holder->weight < e->weight) {
							e_holder = e;
						}
						continue;
					}
				}
				else {
					int i;
//					int j = (boost::regex_match(name_, boost::regex("^sb[fk]s|sbxh|sbzr|sbjk|sb[fk]m$"))) ? 2 : 3;
					int j = 3;
					if (forced_selection_ && !single_selection_)
						j += 5;
					for (i = 0; i < j; i++) {
						if (e->text == string(words[i]))
							break;
					}
					if (i < j)
						continue;
					int l = len == 6 ? 3 : 8;
					if (boost::regex_match(name_, boost::regex("^sbjm|sbdp$")) && enable_filtering_ && string("aeuio").find(input[l]) != string::npos
						&& 9 <= utf8::unchecked::distance(e->text.c_str(), e->text.c_str() + e->text.length()))
						continue;
					else
						result->Add(e);
				}
			}
			else if (boost::regex_match(name_, boost::regex("^sb[fk]x$")) && (len == 7 || (prefixed && len == 12))) {
				int i;
				int j = 2;
				if (forced_selection_ && !single_selection_)
					j += 5;
				for (i = 0; i < j; i++) {
					if (e->text == string(words[i]))
						break;
				}
				if (i < j)
					continue;
				if (enable_filtering_ && 9 <= utf8::unchecked::distance(e->text.c_str(), e->text.c_str() + e->text.length()))
					continue;
				else
					result->Add(e);
			}
			else {
				if (prefixed && delete_threshold_ > 0) {
					if (!DeleteEntry(e))
						result->Add(e);
					else
						continue;
				}
				else
					result->Add(e);
			}

			++count;
			if (is_exact_match)
				++exact_match_count;
			else if (limit && count >= limit)
				break;
		}
		if (e_holder && result->size() < 1) {	// found one most used entry
			++count;
			++exact_match_count;
			if (boost::regex_match(name_, boost::regex("^sb[fk]x$"))) {
				if (len == 5 || (prefixed && len == 10))
					std::strcpy(words[0], e_holder->text.c_str());
				else if (len == 6 || (prefixed && len == 11))
					std::strcpy(words[1], e_holder->text.c_str());
			}
			else {
				if (len == 3 || (prefixed && len == 8))
					std::strcpy(words[0], e_holder->text.c_str());
				else if (len == 4 || (prefixed && len == 9))
					std::strcpy(words[1], e_holder->text.c_str());
				else if (len == 5 || (prefixed && len == 10))
					std::strcpy(words[2], e_holder->text.c_str());
			}
			result->Add(e_holder);
		}
		if (exact_match_count > 0) {
			result->SortRange(start, exact_match_count);
		}
		if (boost::regex_match(name_, boost::regex("^sbxh|sbzr|sbj[mk]|sb[fk]m|sb[fk]s$")) && prefixed && len == 9 && result->size() > 0 && !single_selection_) {
			int i = 1;
			while (words[i] != string("")) {
				result->Next();
				i++;
			}
			if (i < 7 && result->size() >= i) {
				while (i < 7) {
					auto en = result->Peek();
					if (!en)
						break;
					std::strcpy(words[i], en->text.c_str());
					result->Next();
					i++;
				}
				while (i < 7) {
					std::strcpy(words[i], "");
					i++;
				}
			}
			result->SetIndex(0);
		}
		else if (boost::regex_match(name_, boost::regex("^sbxh|sbzr|sbj[mk]|sb[fk]m|sb[fk]s$")) && len == 4 && result->size() > 0 && !single_selection_) {
			int i = 1;
			while (i < 7) {
				auto en = result->Peek();
				if (!en)
					break;
				std::strcpy(words[i], en->text.c_str());
				result->Next();
				i++;
			}
			while (i < 7) {
				std::strcpy(words[i], "");
				i++;
			}
			result->SetIndex(0);
		}
		else if (boost::regex_match(name_, boost::regex("^sb[fk]x$")) && prefixed && len == 10 && result->size() > 0 && !single_selection_) {
			int i = 1;
			while (words[i] != string("")) {
				result->Next();
				i++;
			}
			if (i < 7 && result->size() >= i) {
				while (i < 7) {
					auto en = result->Peek();
					if (!en)
						break;
					std::strcpy(words[i], en->text.c_str());
					result->Next();
					i++;
				}
				while (i < 7) {
					std::strcpy(words[i], "");
					i++;
				}
			}
			result->SetIndex(0);
		}
		else if (boost::regex_match(name_, boost::regex("^sb[fk]x$")) && len == 5 && result->size() > 0 && !single_selection_) {
			int i = 1;
			while (i < 7) {
				auto en = result->Peek();
				if (!en)
					break;
				std::strcpy(words[i], en->text.c_str());
				result->Next();
				i++;
			}
			while (i < 7) {
				std::strcpy(words[i], "");
				i++;
			}
			result->SetIndex(0);
		}
		if (resume_key) {
			*resume_key = key;
			DLOG(INFO) << "resume key reset to: " << *resume_key;
		}
		return count;
	}

	bool UserDictionary::UpdateEntry(const DictEntry& entry, int commits) {
		return UpdateEntry(entry, commits, "");
	}

	bool UserDictionary::UpdateEntry(const DictEntry& entry, int commits,
		const string& new_entry_prefix) {
		string code_str(entry.custom_code);
		if (code_str.empty() && !TranslateCodeToString(entry.code, &code_str))
			return false;
		string key(code_str + '\t' + entry.text);
		string value;
		UserDbValue v;
		if (db_->Fetch(key, &value)) {
			v.Unpack(value);
			if (v.tick > tick_) {
				v.tick = tick_;  // fix abnormal timestamp
			}
			if (v.commits < 0)
				v.commits = -v.commits;
			else if (!new_entry_prefix.empty()) // April 12, 2021, not increase commits of existing entries  
				return false;
		}
		else if (!new_entry_prefix.empty()) {
			key.insert(0, new_entry_prefix);
		}
		if (commits > 0) {
			if (v.commits < 0)
				v.commits = -v.commits;  // revive a deleted item
			v.commits += commits;
			UpdateTickCount(1);
			v.dee = algo::formula_d(commits, (double)tick_, v.dee, (double)v.tick);
		}
		else if (commits == 0) {
			const double k = 0.1;
			v.dee = algo::formula_d(k, (double)tick_, v.dee, (double)v.tick);
		}
		else if (commits < 0) {  // mark as deleted
			v.commits = (std::min)(-1, -v.commits);
			v.dee = algo::formula_d(0.0, (double)tick_, v.dee, (double)v.tick);
		}
		v.tick = tick_;
		return db_->Update(key, v.Pack());
	}

	bool UserDictionary::DeleteEntry(an<DictEntry> entry) {
		string code_str(entry->custom_code);
		if (code_str.empty() && !TranslateCodeToString(entry->code, &code_str))
			return false;
		string key(code_str + '\t' + entry->text);
		string value;
		UserDbValue v;
		if (db_->Fetch(key, &value)) {
			v.Unpack(value);
			if (tick_ - v.tick >= delete_threshold_ ) {
				v.commits = -1;
				v.dee = algo::formula_d(0.0, (double)tick_, v.dee, (double)v.tick);
				return db_->Update(key, v.Pack());
			}
		}
		return false;
	}

	bool UserDictionary::UpdateTickCount(TickCount increment) {
		tick_ += increment;
		try {
			return db_->MetaUpdate("/tick", boost::lexical_cast<string>(tick_));
		}
		catch (...) {
			return false;
		}
	}

	bool UserDictionary::Initialize() {
		return db_->MetaUpdate("/tick", "0");
	}

	bool UserDictionary::FetchTickCount() {
		string value;
		try {
			// an earlier version mistakenly wrote tick count into an empty key
			if (!db_->MetaFetch("/tick", &value) &&
				!db_->Fetch("", &value))
				return false;
			tick_ = boost::lexical_cast<TickCount>(value);
			return true;
		}
		catch (...) {
			//tick_ = 0;
			return false;
		}
	}

	bool UserDictionary::NewTransaction() {
		auto db = As<Transactional>(db_);
		if (!db)
			return false;
		CommitPendingTransaction();
		transaction_time_ = time(NULL);
		return db->BeginTransaction();
	}

	bool UserDictionary::RevertRecentTransaction() {
		auto db = As<Transactional>(db_);
		if (!db || !db->in_transaction())
			return false;
		if (time(NULL) - transaction_time_ > 3/*seconds*/)
			return false;
		return db->AbortTransaction();
	}

	bool UserDictionary::CommitPendingTransaction() {
		auto db = As<Transactional>(db_);
		if (db && db->in_transaction()) {
			return db->CommitTransaction();
		}
		return false;
	}

	bool UserDictionary::TranslateCodeToString(const Code& code,
		string* result) {
		if (!table_ || !result) return false;
		result->clear();
		for (const SyllableId& syllable_id : code) {
			string spelling = table_->GetSyllableById(syllable_id);
			if (spelling.empty()) {
				LOG(ERROR) << "Error translating syllable_id '" << syllable_id << "'.";
				result->clear();
				return false;
			}
			*result += spelling;
			*result += ' ';
		}
		return true;
	}

	an<DictEntry> UserDictionary::CreateDictEntry(const string& key,
		const string& value,
		TickCount present_tick,
		double credibility,
		string* full_code) {
		an<DictEntry> e;
		size_t separator_pos = key.find('\t');
		if (separator_pos == string::npos)
			return e;
		UserDbValue v;
		if (!v.Unpack(value))
			return e;
		if (v.commits < 0)  // deleted entry
			return e;
		if (v.tick < present_tick)
			v.dee = algo::formula_d(0, (double)present_tick, v.dee, (double)v.tick);
		// create!
		e = New<DictEntry>();
		e->text = key.substr(separator_pos + 1);
		e->commit_count = v.commits;
		// TODO: argument s not defined...
		double weight = algo::formula_p(0,
			(double)v.commits / present_tick,
			(double)present_tick,
			v.dee);
		e->weight = log(weight > 0 ? weight : DBL_EPSILON) + credibility;
		if (full_code) {
			*full_code = key.substr(0, separator_pos);
		}
		DLOG(INFO) << "text = '" << e->text
			<< "', code_len = " << e->code.size()
			<< ", weight = " << e->weight
			<< ", commit_count = " << e->commit_count
			<< ", present_tick = " << present_tick;
		return e;
	}

	// UserDictionaryComponent members

	UserDictionaryComponent::UserDictionaryComponent() {
	}

	UserDictionary* UserDictionaryComponent::Create(const Ticket& ticket) {
		if (!ticket.schema)
			return NULL;
		Config* config = ticket.schema->config();
		bool enable_user_dict = true;
		config->GetBool(ticket.name_space + "/enable_user_dict", &enable_user_dict);
		if (!enable_user_dict)
			return NULL;
		string dict_name;
		if (config->GetString(ticket.name_space + "/user_dict", &dict_name)) {
			// user specified name
		}
		else if (config->GetString(ticket.name_space + "/dictionary", &dict_name)) {
			// {dictionary: luna_pinyin.extra} implies {user_dict: luna_pinyin}
			dict_name = Language::get_language_component(dict_name);
		}
		else {
			LOG(ERROR) << ticket.name_space << "/dictionary not specified in schema '"
				<< ticket.schema->schema_id() << "'.";
			return NULL;
		}
		string db_class("userdb");
		if (config->GetString(ticket.name_space + "/db_class", &db_class)) {
			// user specified db class
		}
		// obtain userdb object
		auto db = db_pool_[dict_name].lock();
		if (!db) {
			auto component = Db::Require(db_class);
			if (!component) {
				LOG(ERROR) << "undefined db class '" << db_class << "'.";
				return NULL;
			}
			db.reset(component->Create(dict_name));
			db_pool_[dict_name] = db;
		}

		int delete_threshold = 1000;
		config->GetInt(ticket.name_space + "/delete_threshold", &delete_threshold);
		if (delete_threshold < 0)
			delete_threshold = 0;
		bool enable_filtering = false;
		config->GetBool(ticket.name_space + "/enable_filtering", &enable_filtering);
		bool forced_selection = true;
		config->GetBool(ticket.name_space + "/forced_selection", &forced_selection);
		bool single_selection = false;
		config->GetBool(ticket.name_space + "/single_selection", &single_selection);
		bool strong_mode = false;
		config->GetBool(ticket.name_space + "/strong_mode", &strong_mode);
		bool lower_case = false;
		config->GetBool(ticket.name_space + "/lower_case", &lower_case);

		return new UserDictionary(dict_name, db, ticket.schema->schema_id(), delete_threshold, enable_filtering, forced_selection, single_selection, strong_mode, lower_case);
	}

}  // namespace rime
