//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-10-27 GONG Chen <chen.sst@gmail.com>
//
#include <utility>
#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/key_table.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/gear/speller.h>

static const char kRimeAlphabet[] = "zyxwvutsrqponmlkjihgfedcba";

namespace rime {

    static inline bool belongs_to(char ch, const string &charset) {
        return charset.find(ch) != string::npos;
    }

    static bool reached_max_code_length(const an<Candidate> &cand,
                                        int max_code_length) {
        if (!cand)
            return false;
        int code_length = static_cast<int>(cand->end() - cand->start());
        return code_length >= max_code_length;
    }

    static inline bool is_table_entry(const an<Candidate> &cand) {
        const auto &type = Candidate::GetGenuineCandidate(cand)->type();
        return type == "table" || type == "user_table";
    }

    static bool is_auto_selectable(const an<Candidate> &cand,
                                   const string &input,
                                   const string &delimiters) {
        return
            // reaches end of input
                cand->end() == input.length() &&
                is_table_entry(cand) &&
                // no delimiters
                input.find_first_of(delimiters, cand->start()) == string::npos;
    }

    static bool expecting_an_initial(Context *ctx,
                                     const string &alphabet,
                                     const string &finals) {
        size_t caret_pos = ctx->caret_pos();
        if (caret_pos == 0 ||
            caret_pos == ctx->composition().GetCurrentStartPosition()) {
            return true;
        }
        const string &input(ctx->input());
        char previous_char = input[caret_pos - 1];
        return belongs_to(previous_char, finals) ||
               !belongs_to(previous_char, alphabet);
    }

    Speller::Speller(const Ticket &ticket) : Processor(ticket),
                                             alphabet_(kRimeAlphabet) {
        if (Config *config = engine_->schema()->config()) {
            config->GetString("speller/alphabet", &alphabet_);
            config->GetString("speller/delimiter", &delimiters_);
            config->GetString("speller/initials", &initials_);
            config->GetString("speller/finals", &finals_);
            config->GetInt("speller/max_code_length", &max_code_length_);
            config->GetBool("speller/auto_select", &auto_select_);
            config->GetBool("speller/use_space", &use_space_);
            string pattern;
            if (config->GetString("speller/auto_select_pattern", &pattern)) {
                auto_select_pattern_ = pattern;
            }
            string auto_clear;
            if (config->GetString("speller/auto_clear", &auto_clear)) {
                if (auto_clear == "auto") auto_clear_ = kClearAuto;
                else if (auto_clear == "manual") auto_clear_ = kClearManual;
                else if (auto_clear == "max_length") auto_clear_ = kClearMaxLength;
            }
        }
        if (initials_.empty()) {
            initials_ = alphabet_;
        }
    }

    ProcessResult Speller::ProcessKeyEvent(const KeyEvent &key_event) {
        if (key_event.release() || key_event.ctrl() || key_event.alt())
            return kNoop;
        int ch = key_event.keycode();
        if (ch < 0x20 || ch >= 0x7f)  // not a valid key for spelling
            return kNoop;
        if (ch == XK_space && (!use_space_ || key_event.shift()))
            return kNoop;
        if (!belongs_to(ch, alphabet_) && !belongs_to(ch, delimiters_))
            return kNoop;
        Context *ctx = engine_->context();
        bool is_initial = belongs_to(ch, initials_);
        if (!is_initial &&
            expecting_an_initial(ctx, alphabet_, finals_)) {
            return kNoop;
        }

        string schema = engine_->schema()->schema_id();
		Composition comp = ctx->composition();
		size_t comfirmed_pos = comp.GetConfirmedPosition();
		size_t len = ctx->input().length() - comfirmed_pos;
		const char c1 = ctx->input()[comfirmed_pos];
		bool is_sbxlm = boost::regex_match(schema, boost::regex("^sb[fk][mxd]|sbjm|sbdp|sbzr|sbxh|sbpy|sb[fkhzjd]z$"));
		bool pro_char = ctx->get_option("pro_char") && boost::regex_match(schema, boost::regex("^sb[fk][mxd]|sbzr|sbxh$"));
		bool fine_sentence = ctx->get_option("_auto_commit") && boost::regex_match(schema, boost::regex("^sb[fk][mxd]|sbjm|sbdp|sbzr|sbxh$"));
		bool is_enhanced = ctx->get_option("is_enhanced") && boost::regex_match(schema, boost::regex("^sb[fk][mxd]|sbzr|sbxh|sbjm|sbdp$"));
		bool third_pop = ctx->get_option("third_pop");

        if (len == 1 && !islower(c1) && is_sbxlm) {
			ctx->ConfirmCurrentSelection();
			if (!fine_sentence) {
				ctx->Commit();
				ctx->Clear();
			}
			ctx->PushInput(ch);
			return kAccepted;
		}

		if (is_initial && pro_char && 2 == len && is_sbxlm && belongs_to(c1, initials_)) {
			ctx->ConfirmCurrentSelection();
			if (!fine_sentence) {
				ctx->Commit();
				ctx->Clear();
			}
			ctx->PushInput(ch);
			return kAccepted;
		}

		if (isupper(ch) && pro_char && 2 == len && is_sbxlm && belongs_to(c1, initials_)) {
			ctx->PushInput(tolower(ch));
			return kAccepted;
		}

		if (isdigit(ch) && is_enhanced && 2 == len && belongs_to(c1, initials_) 
				&& string("aeuio1234567890").find(ctx->input()[comfirmed_pos + 1]) == string::npos) {
			if (fine_sentence) {
				ctx->set_caret_pos(ctx->caret_pos() - 1);
				ctx->ConfirmCurrentSelection();
				ctx->set_caret_pos(ctx->caret_pos() + 1);
			}
			else {
				string rest = ctx->input().substr(1, 1);
				ctx->set_input(ctx->input().substr(0, 1));
				ctx->ConfirmCurrentSelection();
				ctx->Commit();
				ctx->set_input(rest);
			}
			ctx->PushInput(tolower(ch));
			return kAccepted;
		}

		if (isdigit(ch) && is_enhanced && 3 == len && belongs_to(c1, initials_)
			&& string("aeuio1234567890").find(ctx->input()[comfirmed_pos + 2]) == string::npos) {
			if (fine_sentence) {
				ctx->set_caret_pos(ctx->caret_pos() - 1);
				ctx->ConfirmCurrentSelection();
				ctx->set_caret_pos(ctx->caret_pos() + 1);
			}
			else {
				string rest = ctx->input().substr(2, 1);
				ctx->set_input(ctx->input().substr(0, 2));
				ctx->ConfirmCurrentSelection();
				ctx->Commit();
				ctx->set_input(rest);
			}
			ctx->PushInput(tolower(ch));
			return kAccepted;
		}

		if (string("AEUIO").find(ch) != string::npos && 3 == len
			&& boost::regex_match(schema, boost::regex("^sb[fk][mxd]|sbzr|sbxh$"))) {
			if (fine_sentence) {
				ctx->set_caret_pos(ctx->caret_pos() - 2);
				ctx->ConfirmCurrentSelection();
				ctx->set_caret_pos(ctx->caret_pos() + 2);
			}
			else {
				string rest = ctx->input().substr(1, 2);
				ctx->set_input(ctx->input().substr(0, 1));
				ctx->ConfirmCurrentSelection();
				ctx->Commit();
				ctx->set_input(rest);
			}
			ctx->PushInput(tolower(ch));
			return kAccepted;
		}

        if (is_initial && 3 == len && belongs_to(c1, initials_)
			&& (third_pop && boost::regex_match(schema, boost::regex("^sbjm|sbdp$")))
			&& string("aeuio").find(ch) == string::npos) {
            ctx->ConfirmCurrentSelection();
			if (!fine_sentence) {
				ctx->Commit();
				ctx->Clear();
			}
            ctx->PushInput(ch);
            return kAccepted;
        }

        if (isupper(ch) && 3 == len && belongs_to(c1, initials_)
			&& (third_pop && boost::regex_match(schema, boost::regex("^sbjm|sbdp$")))) {
			ctx->PushInput(tolower(ch));
			return kAccepted;
        }

		if (isupper(ch) && is_sbxlm && 3 >= len && belongs_to(c1, initials_)) {
			ctx->ConfirmCurrentSelection();
			if (!fine_sentence)
				ctx->Commit();
			ctx->PushInput(tolower(ch));
			return kAccepted;
		}

		if (3 == len && belongs_to(c1, initials_)
			&& string("qwrtsdfgzxcvbyphjklnm").find(ctx->input()[comfirmed_pos + 2]) != string::npos
			&& string("qwrtsdfgzxcvbyphjklnm").find(ch) != string::npos
			&& boost::regex_match(schema, boost::regex("^sb[fk]x$"))) {
			if (fine_sentence) {
				ctx->set_caret_pos(ctx->caret_pos() - 1);
				ctx->ConfirmCurrentSelection();
				ctx->set_caret_pos(ctx->caret_pos() + 1);
			}
			else {
				string rest = ctx->input().substr(2, 1);
				ctx->set_input(ctx->input().substr(0, 2));
				ctx->ConfirmCurrentSelection();
				ctx->Commit();
				ctx->set_input(rest);
			}
			ctx->PushInput(ch);
			return kAccepted;
		}

        if (4 == len && isupper(ch) && belongs_to(c1, initials_)
            && string("aeuio").find(ctx->input()[comfirmed_pos + 2]) == string::npos
            && boost::regex_match(schema, boost::regex("^sb[fk][mx]|sbzr|sbxh|sbjm|sbdp$"))) {
			if (fine_sentence) {
				ctx->set_caret_pos(ctx->caret_pos() - 2);
				ctx->ConfirmCurrentSelection();
				ctx->set_caret_pos(ctx->caret_pos() + 2);
			}
			else {
				string rest = ctx->input().substr(2, 2);
				ctx->set_input(ctx->input().substr(0, 2));
				ctx->ConfirmCurrentSelection();
				ctx->Commit();
				ctx->set_input(rest);
			}
            ctx->PushInput(tolower(ch));
            return kAccepted;
        }



		if (5 == len && isupper(ch) && belongs_to(c1, initials_)
			&& string("aeuio").find(ctx->input()[comfirmed_pos + 2]) == string::npos
			&& boost::regex_match(schema, boost::regex("^sb[fk]x$"))) {
			if (string("AEUIO").find(ch) == string::npos)
				ctx->PushInput(tolower(ch));
			else if (fine_sentence) {
				ctx->set_caret_pos(ctx->caret_pos() - 3);
				ctx->ConfirmCurrentSelection();
				ctx->set_caret_pos(ctx->caret_pos() + 3);
			}
			else {
				string rest = ctx->input().substr(2, 3);
				ctx->set_input(ctx->input().substr(0, 2));
				ctx->ConfirmCurrentSelection();
				ctx->Commit();
				ctx->set_input(rest);
				ctx->ConfirmCurrentSelection();
				ctx->Commit();
			}
			return kAccepted;
		}

        // handles input beyond max_code_length when auto_select is false.
        if (is_initial && AutoSelectAtMaxCodeLength(ctx)) {
            DLOG(INFO) << "auto-select at max code length.";
        } else if ((auto_clear_ == kClearMaxLength || auto_clear_ == kClearManual) && AutoClear(ctx)) {
            DLOG(INFO) << "auto-clear at max code when no candidate.";
        }
        // make a backup of previous conversion before modifying input
        Segment previous_segment;
        if (auto_select_ && ctx->HasMenu()) {
            previous_segment = ctx->composition().back();
        }
        DLOG(INFO) << "add to input: '" << (char) ch << "', " << key_event.repr();
        ctx->PushInput(ch);
		bool is_sbjm = boost::regex_match(engine_->schema()->schema_id(), boost::regex("^sbjm|sbdp$"));
		if (is_sbjm && third_pop && len == 4
			&& string("aeuio\\").find(c1) == string::npos
			&& string("aeuio").find(ctx->input()[comfirmed_pos + 3]) != string::npos)
			return kAccepted;
		ctx->ConfirmPreviousSelection();  // so that next BackSpace won't revert
        // previous selection
        if (AutoSelectPreviousMatch(ctx, &previous_segment)) {
            DLOG(INFO) << "auto-select previous match.";
            // after auto-selecting, if only the current non-initial key is left,
            // then it should be handled by other processors.
            if (!is_initial && ctx->composition().GetCurrentSegmentLength() == 1) {
                ctx->PopInput(1);
                return kNoop;
            }
        }
        if (AutoSelectUniqueCandidate(ctx)) {
            DLOG(INFO) << "auto-select unique candidate.";
        } else if (auto_clear_ == kClearAuto && AutoClear(ctx)) {
            DLOG(INFO) << "auto-clear when no candidate.";
        }

        return kAccepted;
    }

    bool Speller::AutoSelectAtMaxCodeLength(Context *ctx) {
        if (max_code_length_ <= 0)
            return false;
        if (!ctx->HasMenu())
            return false;
        auto cand = ctx->GetSelectedCandidate();
		int mcl = ctx->get_option("_auto_commit") ? max_code_length_ : 255;
        if (cand &&
            reached_max_code_length(cand, mcl) &&
            is_auto_selectable(cand, ctx->input(), delimiters_)) {
            ctx->ConfirmCurrentSelection();
            return true;
        }
        return false;
    }

    bool Speller::AutoSelectUniqueCandidate(Context *ctx) {
        if (!auto_select_)
            return false;
        if (!ctx->HasMenu())
            return false;
        const Segment &seg(ctx->composition().back());
        bool unique_candidate = seg.menu->Prepare(2) == 1;
        if (!unique_candidate)
            return false;
        const string &input(ctx->input());
        auto cand = seg.GetSelectedCandidate();
        bool matches_input_pattern = false;
        if (auto_select_pattern_.empty()) {
            matches_input_pattern =
                    max_code_length_ == 0 ||  // match any length if not set
                    reached_max_code_length(cand, max_code_length_);
        } else {
            string code(input.substr(cand->start(), cand->end()));
            matches_input_pattern = boost::regex_match(code, auto_select_pattern_);
        }
        if (matches_input_pattern &&
            is_auto_selectable(cand, input, delimiters_)) {
            ctx->ConfirmCurrentSelection();
            return true;
        }
        return false;
    }

    bool Speller::AutoSelectPreviousMatch(Context *ctx,
                                          Segment *previous_segment) {
        if (!auto_select_)
            return false;
        if (!auto_select_pattern_.empty())
            return false;
        if (ctx->HasMenu())  // if and only if current conversion fails
            return false;
        if (!previous_segment->menu)
            return false;
        size_t start = previous_segment->start;
        size_t end = previous_segment->end;
        string input = ctx->input();
		string converted = input.substr(0, end);
		auto cand = previous_segment->GetSelectedCandidate();

		string schema = engine_->schema()->schema_id();
		Composition comp = ctx->composition();
		size_t comfirmed_pos = comp.GetConfirmedPosition();
		size_t len = ctx->input().length() - comfirmed_pos;

		if (5 == len && is_table_entry(cand)
			&& string("QWRTSDFGZXCVBYPHJKLNM").find(input[comfirmed_pos + 3]) == string::npos
			&& !(string("aeuio").find(input[comfirmed_pos + 1]) != string::npos &&
				string("aeuio").find(input[comfirmed_pos + 2]) != string::npos)
			&& !(string("aeuio").find(input[comfirmed_pos + 1]) == string::npos &&
				string("aeuio").find(input[comfirmed_pos + 2]) != string::npos)
			&& boost::regex_match(schema, boost::regex("^sb[fk]x$"))) {
			return FindEarlierMatch(ctx, start, end - 1);
        } else if (5 == len && is_table_entry(cand)
                   && string("aeuio").find(input[comfirmed_pos + 4]) != string::npos
                   && !(string("aeuio").find(input[comfirmed_pos + 1]) != string::npos &&
                        string("aeuio").find(input[comfirmed_pos + 2]) != string::npos)
                   && !(string("aeuio").find(input[comfirmed_pos + 1]) == string::npos &&
                        string("aeuio").find(input[comfirmed_pos + 2]) != string::npos)
                   && boost::regex_match(schema, boost::regex("^sb[fk]m|sbzr|sbxh$"))) {
            return FindEarlierMatch(ctx, start, end - 1);
        } else if (is_auto_selectable(previous_segment->GetSelectedCandidate(),
                                      converted, delimiters_)) {
            // reuse previous match
            ctx->composition().pop_back();
            ctx->composition().push_back(std::move(*previous_segment));
            ctx->ConfirmCurrentSelection();
			if (ctx->get_option("_auto_commit")) {
                ctx->set_input(converted);
                ctx->Commit();
                string rest = input.substr(end);
                ctx->set_input(rest);
            }
            return true;
        }
        return FindEarlierMatch(ctx, start, end);
    }

    bool Speller::AutoClear(Context *ctx) {
		int mcl = ctx->get_option("_auto_commit") ? max_code_length_ : 255;
        if (!ctx->HasMenu() && auto_clear_ > kClearNone &&
            (auto_clear_ != kClearMaxLength || mcl == 0 ||
             ctx->input().length() >= (size_t)mcl)) {
            ctx->Clear();
            return true;
        }
        return false;
    }

    bool Speller::FindEarlierMatch(Context *ctx, size_t start, size_t end) {
        if (end <= start + 1)
            return false;
        string input = ctx->input();
        string converted = input;
        while (--end > start) {
            converted.resize(end);
            ctx->set_input(converted);
            if (!ctx->HasMenu())
                break;
            const Segment &segment(ctx->composition().back());
            if (is_auto_selectable(segment.GetSelectedCandidate(),
                                   converted, delimiters_)) {
                // select previous match
                if (ctx->get_option("_auto_commit")) {
                    ctx->Commit();
					ctx->Clear();
                    ctx->set_input(input.substr(end));
                    end = 0;
                } else {
                    ctx->ConfirmCurrentSelection();
                    ctx->set_input(input);
                }
                if (!ctx->HasMenu()) {
                    size_t next_start = ctx->composition().GetCurrentStartPosition();
                    size_t next_end = ctx->composition().GetCurrentEndPosition();
                    if (next_start == end) {
                        // continue splitting
                        FindEarlierMatch(ctx, next_start, next_end);
                    }
                }
                return true;
            }
        }
        ctx->set_input(input);
        return false;
    }

}  // namespace rime
