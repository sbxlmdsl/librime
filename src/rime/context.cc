//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-05-08 GONG Chen <chen.sst@gmail.com>
//
#include <utility>
#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/menu.h>
#include <rime/segmentation.h>

namespace rime {

bool Context::Commit() {
  if (!IsComposing())
    return false;
  // notify the engine and interesting components
  commit_notifier_(this);
  // start over
  Clear();
  if (get_option("is_buffered"))
	  set_option("is_buffered", false);
  return true;
}

string Context::GetCommitText() const {
  if (get_option("dumb"))
    return string();
  return composition_.GetCommitText();
}

string Context::GetScriptText() const {
  return composition_.GetScriptText();
}

static const string kCaretSymbol("\xe2\x80\xb8");  // U+2038 ‸ CARET

string Context::GetSoftCursor() const {
  return get_option("soft_cursor") ? kCaretSymbol : string();
}

Preedit Context::GetPreedit() const {
  return composition_.GetPreedit(input_, caret_pos_, GetSoftCursor());
}

bool Context::IsComposing() const {
  return !input_.empty() || !composition_.empty();
}

bool Context::HasMenu() const {
  if (composition_.empty())
    return false;
  const auto& menu(composition_.back().menu);
  return menu && !menu->empty();
}

bool Context::HasMore() const {
  if (composition_.empty())
    return false;
  auto seg = composition_.back();
  if (seg.length >= 2 && seg.length <= 3
	  && string("aeuio\\").find(input_[0]) == string::npos)
	  return false;
  const auto& menu(seg.menu);
  return menu && menu->candidate_count() > 1;
}

bool Context::MorePage() const {
	if (composition_.empty())
		return false;
	const auto& menu(composition_.back().menu);
	return menu && menu->candidate_count() > 5;
}

// for sbkz and sbfz
int Context::CountLength() const {
  if (composition_.empty())
    return 0;
  if (string("aeuio").find(input_[0]) != string::npos)
	  return 0;
  auto seg = composition_.back();
  int j = 0;
  for (int i = seg.start; i < caret_pos_; i++) {
	if (j == 0) {
		if (islower(input_[i]) && string("aeuio").find(input_[i]) == string::npos)
			j++;
		continue;
	} else if (j == 1) {
      j++;
      continue;
    } else if (j >= 2) {
		if (string("aeuio").find(input_[i]) == string::npos)
			j = 1;
		else
			j++;
		continue;
	}
  }
  return j;
}

// for sbjz
int Context::CountLength2() const {
  if (composition_.empty())
    return false;
  auto seg = composition_.back();
  int j = 0;
  for (int i = seg.start; i < caret_pos_; i++) {
	  if (j == 0) {
		  if (islower(input_[i]) && string("aeuio").find(input_[i]) == string::npos)
			  j++;
		  continue;
	  } else if (j >= 1) {
		  if (string("aeuio").find(input_[i]) == string::npos)
			  j = 1;
		  else
			  j++;
		  continue;
	  }
  }
  return j;
}

bool Context::IsFirst() const {
  return CountLength() == 1;
}

bool Context::IsSecond() const {
  return CountLength() == 2;
}

bool Context::IsThird() const {
  return CountLength() == 3;
}

bool Context::IsFourth() const {
  return CountLength() == 4;
}

bool Context::IsFifth() const {
  return CountLength2() == 5;
}

bool Context::IsSixth() const {
  return CountLength2() == 6;
}

bool Context::IsSelect() const {
  if (composition_.empty())
    return false;
  auto seg = composition_.back();
  return IsFourth() || (IsSecond() && string("_aeuio").find(input_[caret_pos_-1]) != string::npos);
}

bool Context::OkSy() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();

	if (seg.length >= 2 && string("aeuio").find(input_[seg.start + 1]) != string::npos) {
		if (seg.length == 2 && !get_option("single") 
			&& string("aeuio").find(input_[seg.start]) == string::npos) {
			return false;
		}
		if (seg.length == 3 && string("aeuio").find(input_[seg.start + 2]) != string::npos) {
			if (get_option("single")
				&& string("aeuio").find(input_[seg.start]) == string::npos)
				return true;
			return false;
		}
		else if (seg.length >= 4 && string("aeuio").find(input_[seg.start + 3]) == string::npos) {
			if (seg.length == 5 && string("aeuio").find(input_[seg.start + 4]) != string::npos)
				return false;
			else if (seg.length >= 6 && string("aeuio").find(input_[seg.start + 5]) == string::npos) {
				if (seg.length == 7 && string("aeuio").find(input_[seg.start + 6]) != string::npos)
					return false;
				else if (seg.length >= 8 && string("aeuio").find(input_[seg.start + 7]) == string::npos) {
					if (seg.length == 9 && string("aeuio").find(input_[seg.start + 8]) != string::npos)
						return false;
					else if (seg.length >= 10 && string("aeuio").find(input_[seg.start + 9]) == string::npos) {
						if (seg.length == 11 && string("aeuio").find(input_[seg.start + 10]) != string::npos)
							return false;
						else if (seg.length >= 10)
							return false;
					}
				}
			}
		}
		else if (seg.length >= 4 && seg.length % 2 == 0 && seg.length < 9
			&& string("aeuio").find(input_[seg.length - 1]) != string::npos) {
			if (string("aeuio").find(input_[seg.length - 2]) != string::npos)
				return true;
			return false;
		}
		else if (seg.length == 9 && string("aeuio").find(input_[seg.length - 1]) == string::npos) {
			return false;
		}
		else if (seg.length > 9) {
			return false;
		}
		return true;
	}
	return false;
}
bool Context::OkSys() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return seg.length == 3
		&& string("aeuio").find(input_[seg.start + 1]) != string::npos
		&& string("aeuio").find(input_[seg.start + 2]) == string::npos
		;
}
bool Context::OkSyxs() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return seg.length >= 4 && seg.length <= 5
		&& string("aeuio").find(input_[seg.start + 1]) != string::npos
		&& string("aeuio").find(input_[seg.start + 2]) != string::npos
		&& string("aeuio").find(input_[seg.start + 3]) == string::npos
		;
}
bool Context::OkSsy() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return seg.length >= 3
		&& string("aeuio").find(input_[seg.start + 1]) == string::npos
		&& string("aeuio").find(input_[seg.start + 2]) != string::npos;
}
bool Context::OkSssy() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return seg.length >= 4 
		&& string("aeuio").find(input_[seg.start + 1]) == string::npos
		&& string("aeuio").find(input_[seg.start + 2]) == string::npos
		&& string("aeuio").find(input_[seg.start + 3]) != string::npos;
}
bool Context::OkSsss() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return 
		(seg.length == 4
		&& string("aeuio").find(input_[seg.start + 1]) == string::npos
		&& string("aeuio").find(input_[seg.start + 2]) == string::npos
		&& string("aeuio").find(input_[seg.start + 3]) == string::npos)
		||
		(seg.length >= 6
		&& string("aeuio").find(input_[seg.start + 1]) == string::npos
		&& string("aeuio").find(input_[seg.start + 2]) == string::npos
		&& string("aeuio").find(input_[seg.start + 3]) == string::npos
		&& string("aeuio").find(input_[seg.start + 4]) != string::npos
		&& string("aeuio").find(input_[seg.start + 5]) != string::npos)
		;
}

bool Context::OkFirst() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return islower(input_[seg.start]) && seg.length == 1 && string("qwrtsdfgzxcvbyphjklnm").find(input_[seg.start]) != string::npos;
}

bool Context::OkSecond() const {
  if (composition_.empty())
    return false;
  if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
	  return false;
  auto seg = composition_.back();
  return islower(input_[seg.start]) && seg.length == 2 
	  && string("qwrtsdfgzxcvbyphjklnm").find(input_[seg.start+1]) != string::npos;
}

bool Context::OkThird() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return islower(input_[seg.start]) && seg.length == 3 
		&& islower(input_[seg.start+2]);
}

bool Context::OkFourth() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return islower(input_[seg.start]) && seg.length == 4
		&& string("aeuio").find(input_[seg.start + 2]) == string::npos
		&& (islower(input_[3]) 
			|| string("1234567890").find(input_[seg.start + 3]) != string::npos
			&& get_option("is_enhanced"));
}

bool Context::OkFifth() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return islower(input_[seg.start]) && seg.length == 5
		&& string("aeuio").find(input_[seg.start + 2]) == string::npos
		&& islower(input_[seg.start + 3]);
}

bool Context::FourthDigit() const {
	if (composition_.empty())
		return false;
	if (input_.length() > 0 && string("aeuio").find(input_[0]) != string::npos)
		return false;
	auto seg = composition_.back();
	return seg.length == 4 && get_option("fast_pop") && !get_option("is_enhanced")
		&& string("1234567890").find(input_[seg.length - 1]) != string::npos
		&& string("aeuio").find(input_[seg.start]) == string::npos;
}

an<Candidate> Context::GetSelectedCandidate() const {
  if (composition_.empty())
    return nullptr;
  return composition_.back().GetSelectedCandidate();
}

bool Context::PushInput(char ch) {
  if (caret_pos_ >= input_.length()) {
    input_.push_back(ch);
    caret_pos_ = input_.length();
  }
  else {
    input_.insert(caret_pos_, 1, ch);
    ++caret_pos_;
  }
  update_notifier_(this);
  return true;
}

bool Context::PushInput(const string& str) {
  if (caret_pos_ >= input_.length()) {
    input_ += str;
    caret_pos_ = input_.length();
  }
  else {
    input_.insert(caret_pos_, str);
    caret_pos_ += str.length();
  }
  update_notifier_(this);
  return true;
}

bool Context::PopInput(size_t len) {
  if (caret_pos_ < len)
    return false;
  caret_pos_ -= len;
  input_.erase(caret_pos_, len);
  update_notifier_(this);
  return true;
}

bool Context::DeleteInput(size_t len) {
  if (caret_pos_ + len > input_.length())
    return false;
  input_.erase(caret_pos_, len);
  update_notifier_(this);
  return true;
}

void Context::Clear() {
  input_.clear();
  caret_pos_ = 0;
  composition_.clear();
  update_notifier_(this);
}

bool Context::Select(size_t index) {
  if (composition_.empty())
    return false;
  Segment& seg(composition_.back());
  if (auto cand = seg.GetCandidateAt(index)) {
    seg.selected_index = index;
    seg.status = Segment::kSelected;
    DLOG(INFO) << "Selected: '" << cand->text() << "', index = " << index;
    select_notifier_(this);
    return true;
  }
  return false;
}

bool Context::DeleteCandidate(
    function<an<Candidate> (Segment& seg)> get_candidate) {
  if (composition_.empty())
    return false;
  Segment& seg(composition_.back());
  if (auto cand = get_candidate(seg)) {
    DLOG(INFO) << "Deleting candidate: '" << cand->text();
    delete_notifier_(this);
    return true;  // CAVEAT: this doesn't mean anything is deleted for sure
  }
  return false;
}

bool Context::DeleteCandidate(size_t index) {
  return DeleteCandidate(
      [index](Segment& seg) {
        return seg.GetCandidateAt(index);
      });
}

bool Context::DeleteCurrentSelection() {
  return DeleteCandidate(
      [](Segment& seg) {
        return seg.GetSelectedCandidate();
      });
}

bool Context::ConfirmCurrentSelection() {
  if (composition_.empty())
    return false;
  Segment& seg(composition_.back());
  seg.status = Segment::kSelected;
  if (auto cand = seg.GetSelectedCandidate()) {
    DLOG(INFO) << "Confirmed: '" << cand->text()
               << "', selected_index = " << seg.selected_index;
  }
  else {
    if (seg.end == seg.start) {
      // fluid_editor will confirm the whole sentence
      return false;
    }
    // confirm raw input
  }
  select_notifier_(this);
  return true;
}

bool Context::ConfirmPreviousSelection() {
  for (auto it = composition_.rbegin(); it != composition_.rend(); ++it) {
    if (it->status > Segment::kSelected) {
      return false;
    }
    if (it->status == Segment::kSelected) {
      it->status = Segment::kConfirmed;
      return true;
    }
  }
  return false;
}

bool Context::ReopenPreviousSegment() {
  if (composition_.Trim()) {
    if (!composition_.empty() &&
        composition_.back().status >= Segment::kSelected) {
      composition_.back().Reopen(caret_pos());
    }
    update_notifier_(this);
    return true;
  }
  return false;
}

bool Context::ClearPreviousSegment() {
  if (composition_.empty())
    return false;
  size_t where = composition_.back().start;
  if (where >= input_.length())
    return false;
  set_input(input_.substr(0, where));
  return true;
}

bool Context::ReopenPreviousSelection() {
  for (auto it = composition_.rbegin(); it != composition_.rend(); ++it) {
    if (it->status > Segment::kSelected)
      return false;
    if (it->status == Segment::kSelected) {
      while (it != composition_.rbegin()) {
        composition_.pop_back();
      }
      it->Reopen(caret_pos());
      update_notifier_(this);
      return true;
    }
  }
  return false;
}

bool Context::ClearNonConfirmedComposition() {
  bool reverted = false;
  while (!composition_.empty() &&
         composition_.back().status < Segment::kSelected) {
    composition_.pop_back();
    reverted = true;
  }
  if (reverted) {
    composition_.Forward();
    DLOG(INFO) << "composition: " << composition_.GetDebugText();
  }
  return reverted;
}

bool Context::RefreshNonConfirmedComposition() {
  if (ClearNonConfirmedComposition()) {
    update_notifier_(this);
    return true;
  }
  return false;
}

void Context::set_caret_pos(size_t caret_pos) {
  if (caret_pos > input_.length())
    caret_pos_ = input_.length();
  else
    caret_pos_ = caret_pos;
  update_notifier_(this);
}

void Context::set_composition(Composition&& comp) {
  composition_ = std::move(comp);
}

void Context::set_input(const string& value) {
  input_ = value;
  caret_pos_ = input_.length();
  update_notifier_(this);
}

void Context::set_option(const string& name, bool value) {
  options_[name] = value;
  option_update_notifier_(this, name);

  if (name == "is_buffered") {
	  if (value)
		options_["_auto_commit"] = false;
	  else
		options_["_auto_commit"] = true;

	  option_update_notifier_(this, "_auto_commit");
  }
}

bool Context::get_option(const string& name) const {
  auto it = options_.find(name);
  if (it != options_.end())
    return it->second;
  else
    return false;
}

void Context::set_property(const string& name,
                           const string& value) {
  properties_[name] = value;
  property_update_notifier_(this, name);
}

string Context::get_property(const string& name) const {
  auto it = properties_.find(name);
  if (it != properties_.end())
    return it->second;
  else
    return string();
}

void Context::ClearTransientOptions() {
  auto opt = options_.lower_bound("_");
  while (opt != options_.end() &&
         !opt->first.empty() && opt->first[0] == '_') {
    options_.erase(opt++);
  }
  auto prop = properties_.lower_bound("_");
  while (prop != properties_.end() &&
         !prop->first.empty() && prop->first[0] == '_') {
    properties_.erase(prop++);
  }
}

}  // namespace rime
