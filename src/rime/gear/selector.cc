//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-09-11 GONG Chen <chen.sst@gmail.com>
//
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/key_table.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/gear/selector.h>
#include <boost/regex.hpp>


namespace rime {
    
    Selector::Selector(const Ticket& ticket) : Processor(ticket) {
    }
    
    ProcessResult Selector::ProcessKeyEvent(const KeyEvent& key_event) {
        if (key_event.release() || key_event.alt())
            return kNoop;
        Context* ctx = engine_->context();
        if (ctx->composition().empty())
            return kNoop;
        Segment& current_segment(ctx->composition().back());
        if (!current_segment.menu || current_segment.HasTag("raw"))
            return kNoop;
        int ch = key_event.keycode();
        if (ch == XK_Prior || ch == XK_KP_Prior) {
            PageUp(ctx);
            return kAccepted;
        }
        if (ch == XK_Next || ch == XK_KP_Next) {
            PageDown(ctx);
            return kAccepted;
        }
        if (ch == XK_Up || ch == XK_KP_Up) {
            if (ctx->get_option("_horizontal")) {
                PageUp(ctx);
            } else {
                CursorUp(ctx);
            }
            return kAccepted;
        }
        if (ch == XK_Down || ch == XK_KP_Down) {
            if (ctx->get_option("_horizontal")) {
                PageDown(ctx);
            } else {
                CursorDown(ctx);
            }
            return kAccepted;
        }
        if (ch == XK_Left || ch == XK_KP_Left) {
            if (!key_event.ctrl() &&
                !key_event.shift() &&
                ctx->caret_pos() == ctx->input().length() &&
                ctx->get_option("_horizontal") &&
                CursorUp(ctx)) {
                return kAccepted;
            }
            return kNoop;
        }
        if (ch == XK_Right || ch == XK_KP_Right) {
            if (!key_event.ctrl() &&
                !key_event.shift() &&
                ctx->caret_pos() == ctx->input().length() &&
                ctx->get_option("_horizontal")) {
                CursorDown(ctx);
                return kAccepted;
            }
            return kNoop;
        }
        if (ch == XK_Home || ch == XK_KP_Home) {
            return Home(ctx) ? kAccepted : kNoop;
        }
        if (ch == XK_End || ch == XK_KP_End) {
            return End(ctx) ? kAccepted : kNoop;
        }
        int index = -1;
        const string& select_keys(engine_->schema()->select_keys());
		const char c1 = ctx->input()[0];
		if (!select_keys.empty() && !key_event.ctrl() && ch >= 0x20 && ch < 0x7f) {
            if (!select_keys.compare(" aeuio") && 
				(!ctx->HasMore() || (string("aeuio").find(c1) != string::npos || islower(c1) && ctx->input().length() <= 3))) {
                ; // hack for sbxlm
            }
            else {
                size_t pos = select_keys.find((char)ch);
                if (pos != string::npos) {
                    index = static_cast<int>(pos);
                }
            }
        }
        else if (ch >= XK_0 && ch <= XK_9)
            index = ((ch - XK_0) + 9) % 10;
        else if (ch >= XK_KP_0 && ch <= XK_KP_9)
            index = ((ch - XK_KP_0) + 9) % 10;
        if (index >= 0) {
			if (boost::regex_match(engine_->schema()->schema_id(), boost::regex("^sbjm|sb[fkhz]j|sbxh|sbzr|sbjk|sb[fk]m|sbdp|sb[fk]m[ks]$"))
				&& !current_segment.HasTag("paging") && ctx->input().length() < 6 && islower(ctx->input()[0])) {
				if (boost::regex_match(engine_->schema()->schema_id(), boost::regex("^sb[fk]m$"))
					&& ctx->input().length() == 4 && string("aeuio_").find(ctx->input()[1]) != string::npos
					&& string("qwrtsdfgzxcvbyphjklnm").find(ctx->input()[3]) != string::npos
					)
					return kNoop;
				else if (boost::regex_match(engine_->schema()->schema_id(), boost::regex("^sbxh|sbzr|sb[fk]m|sb[fkhz]j$"))
					&& ctx->input().length() == 4 && string("aeuio").find(ctx->input()[2]) != string::npos)
					;
				else
					return kNoop;
			}
          
			if (boost::regex_match(engine_->schema()->schema_id(), boost::regex("^sb[fk]s|sb[hz]s$"))
				&& !current_segment.HasTag("paging") && ctx->input().length() < 6 && islower(ctx->input()[0])
				&& ctx->input().length() > 3 && string(",;/.'QWRTSDFGZXCVBYPHJKLNM").find(ctx->input()[3]) != string::npos)
				return kNoop;

			if (boost::regex_match(engine_->schema()->schema_id(), boost::regex("^sb[fk]x$"))
				&& !current_segment.HasTag("paging") && ctx->input().length() < 7 && islower(ctx->input()[0])) {
				if (ctx->input().length() == 4 && (string("aeuio").find(ctx->input()[2]) != string::npos
					|| string("QWRTSDFGZXCVBYPHJKLNM").find(ctx->input()[3]) != string::npos))
					;
				else
					return kNoop;
			}

			//if (boost::regex_match(engine_->schema()->schema_id(), boost::regex("^sb[fk][ds]$"))
			//	&& !current_segment.HasTag("paging") && ctx->input().length() < 4 && islower(ctx->input()[0]))
			//	return kNoop;

			if (boost::regex_match(engine_->schema()->schema_id(), boost::regex("^sb[fk][md]|sb[fk]s|sb[hz]s$"))
				&& !current_segment.HasTag("paging") && ctx->input().length() < 4 && islower(ctx->input()[0]))
				return kNoop;

            SelectCandidateAt(ctx, index);
            return kAccepted;
        }
        // not handled
        return kNoop;
    }
    
    bool Selector::PageUp(Context* ctx) {
        Composition& comp = ctx->composition();
        if (comp.empty())
            return false;
        int page_size = engine_->schema()->page_size();
        int selected_index = comp.back().selected_index;
        int index = selected_index < page_size ? 0 : selected_index - page_size;
        comp.back().selected_index = index;
        comp.back().tags.insert("paging");
        return true;
    }
    
    bool Selector::PageDown(Context* ctx) {
        Composition& comp = ctx->composition();
        if (comp.empty() || !comp.back().menu)
            return false;
        int page_size = engine_->schema()->page_size();
        int index = comp.back().selected_index + page_size;
        int page_start = (index / page_size) * page_size;
        int candidate_count = comp.back().menu->Prepare(page_start + page_size);
        if (candidate_count <= page_start)
            return false;
        if (index >= candidate_count)
            index = candidate_count - 1;
        comp.back().selected_index = index;
        comp.back().tags.insert("paging");
        return true;
        
    }
    
    bool Selector::CursorUp(Context* ctx) {
        Composition& comp = ctx->composition();
        if (comp.empty())
            return false;
        int index = comp.back().selected_index;
        if (index <= 0)
            return false;
        comp.back().selected_index = index - 1;
        comp.back().tags.insert("paging");
        return true;
    }
    
    bool Selector::CursorDown(Context* ctx) {
        Composition& comp = ctx->composition();
        if (comp.empty() || !comp.back().menu)
            return false;
        int index = comp.back().selected_index + 1;
        int candidate_count = comp.back().menu->Prepare(index + 1);
        if (candidate_count <= index)
            return false;
        comp.back().selected_index = index;
        comp.back().tags.insert("paging");
        return true;
    }
    
    bool Selector::Home(Context* ctx) {
        if (ctx->composition().empty())
            return false;
        Segment& seg(ctx->composition().back());
        if (seg.selected_index > 0) {
            seg.selected_index = 0;
            return true;
        }
        return false;
    }
    
    bool Selector::End(Context* ctx) {
        if (ctx->caret_pos() < ctx->input().length()) {
            // navigator should handle this
            return false;
        }
        // this is cool:
        return Home(ctx);
    }
    
    
    bool Selector::SelectCandidateAt(Context* ctx, int index) {
        Composition& comp = ctx->composition();
        if (comp.empty())
            return false;
        int page_size = engine_->schema()->page_size();
        if (index >= page_size)
            return false;
        int selected_index = comp.back().selected_index;
        int page_start = (selected_index / page_size) * page_size;
        return ctx->Select(page_start + index);
    }
    
}  // namespace rime
