/*
 * Copyright (C) 2013 Bastian Bloessl <bloessl@ccs-labs.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <ieee802-11/ofdm_sync_long.h>
#include "utils.h"
#include <gnuradio/io_signature.h>
#include <gnuradio/filter/fir_filter.h>
#include <gnuradio/fft/fft.h>

#include <list>
#include <tuple>

using namespace gr::ieee802_11;
using namespace std;


class ofdm_sync_long_impl : public ofdm_sync_long {

public:
ofdm_sync_long_impl(unsigned int sync_length, bool log, bool debug) : block("ofdm_sync_long",
		gr::io_signature::make2(2, 2, sizeof(gr_complex), sizeof(gr_complex)),
		gr::io_signature::make(1, 1, sizeof(gr_complex))),
		d_log(log),
		d_debug(debug),
		d_offset(0),
		d_freq_est(0),
		d_state(SYNC),
		SYNC_LENGTH(sync_length) {

	set_tag_propagation_policy(block::TPP_DONT);
	d_fir = new gr::filter::kernel::fir_filter_ccc(1, LONG);
	d_correlation = gr::fft::malloc_complex(8192);
	d_freq_est_buf = static_cast<gr_complex*>(std::malloc(sync_length * sizeof(gr_complex)));
}

~ofdm_sync_long_impl(){
	delete d_fir;
	gr::fft::free(d_correlation);
	free(d_freq_est_buf);
}

int general_work (int noutput, gr_vector_int& ninput_items,
		gr_vector_const_void_star& input_items,
		gr_vector_void_star& output_items) {

	const gr_complex *in = (const gr_complex*)input_items[0];
	const gr_complex *in_delayed = (const gr_complex*)input_items[1];
	gr_complex *out = (gr_complex*)output_items[0];

	dout << "LONG ninput[0] " << ninput_items[0] << "   ninput[1] " <<
			ninput_items[1] << "  noutput " << noutput <<
			"   state " << d_state << std::endl;

	int ninput = std::min(ninput_items[0], ninput_items[1]);

	const unsigned int nread = nitems_read(0);
	get_tags_in_range(d_tags, 0, nread, nread + ninput);
	if (d_tags.size()) {
		std::sort(d_tags.begin(), d_tags.end(), gr::tag_t::offset_compare);

		const gr::tag_t &tag = d_tags.front();
		const uint64_t offset = tag.offset;

		if(offset > nread) {
			ninput = offset - nread;
		} else {
			if(d_state == COPY) {
				d_state = RESET;
			}
		}
	}


	int i = 0;
	int o = 0;

	switch(d_state) {

	case SYNC:
		d_fir->filterN(d_correlation, in, std::min(SYNC_LENGTH, std::max(ninput - 63, 0)));

		while(i + 63 < ninput) {

			d_freq_est += in[i] * conj(in[i + 16]);
			d_freq_est_buf[d_offset] = d_freq_est;

			d_cor.push_back(std::tuple<double, int>(abs(d_correlation[i]), d_offset));

			i++;
			d_offset++;

			if(d_offset == SYNC_LENGTH) {
				search_frame_start();
				d_freq_est = d_freq_est_buf[std::max<int>(0, d_frame_start - 160 - 17)];
				d_offset = 0;
				d_state = COPY;

				mylog(boost::format("frame at %1% - freq_est (20M): %2%") % d_frame_start
						% ((arg(d_freq_est) / 16) * 20e6 / (2 * M_PI)));
				break;
			}
		}

		break;

	case COPY:
		while(i < ninput) {

			int rel = d_offset - d_frame_start;
			if( (rel >= 0) && (rel % 80 > 15)) {

				if(o >= noutput) {
					break;
				}

				if(rel == 16) {
					assert(o == 0);
					add_item_tag(0, nitems_written(0) + o,
						pmt::string_to_symbol("ofdm_start"),
						pmt::PMT_T,
						pmt::string_to_symbol(name()));
				}

				out[o] = in_delayed[i] * exp(gr_complex(0, d_offset * arg(d_freq_est) / 16));
				o++;

			}

			i++;
			d_offset++;
		}

		break;

	case RESET:
		while(o < noutput) {
			int rel = (d_offset - d_frame_start) % 80;

			if(!rel) {
				d_offset = 0;
				d_freq_est = gr_complex(0, 0);
				d_state = SYNC;
				break;
			} else if(rel > 15) {
				out[o] = 0;
				o++;
			}
			d_offset++;
		}

		break;

	default:
		std::runtime_error("bad state");
		break;
	}

	dout << "produced : " << o << " consumed: " << i << std::endl;

	consume(0, i);
	consume(1, i);
	return o;
}

void forecast (int noutput_items, gr_vector_int &ninput_items_required) {

	// in sync state we need at least a symbol to correlate
	// with the pattern
	if(d_state == SYNC) {
		ninput_items_required[0] = 64;
		ninput_items_required[1] = 64;

	} else {
		ninput_items_required[0] = noutput_items;
		ninput_items_required[1] = noutput_items;
	}
}

void search_frame_start() {

	// sort list (highest correlation first)
	assert(d_cor.size() == SYNC_LENGTH);
	d_cor.sort();
	d_cor.reverse();

	// copy list in vector for nicer access
	vector<tuple<double, int>> vec(d_cor.begin(), d_cor.end());
	d_cor.clear();

	// in case we don't find anything use SYNC_LENGTH
	d_frame_start = SYNC_LENGTH;

	for(int i = 0; i < 3; i++) {
		for(int k = i + 1; k < 4; k++) {
			int diff = abs(get<1>(vec[i]) - get<1>(vec[k]));
			if(diff == 64) {
				d_frame_start =  max(get<1>(vec[i]), get<1>(vec[k])) + 64;
				// nice match found, return immediately
				return;

			// TODO: check if these offsets make sense
			} else if(diff == 63) {
				d_frame_start = max(get<1>(vec[i]), get<1>(vec[k])) + 63;
			} else if(diff = 65) {
				d_frame_start = max(get<1>(vec[i]), get<1>(vec[k])) + 64;
			}
		}
	}
}

private:
	enum {SYNC, COPY, RESET} d_state;
	int         d_offset;
	int         d_frame_start;
	gr_complex  d_freq_est;
	gr_complex *d_correlation;
	gr_complex *d_freq_est_buf;
	std::list<std::tuple<double, int>> d_cor;
	std::vector<gr::tag_t> d_tags;
	gr::filter::kernel::fir_filter_ccc *d_fir;

	const bool d_log;
	const bool d_debug;
	const int  SYNC_LENGTH;

	static const std::vector<gr_complex> LONG;
};

ofdm_sync_long::sptr
ofdm_sync_long::make(unsigned int sync_length, bool log, bool debug) {
	return gnuradio::get_initial_sptr(new ofdm_sync_long_impl(sync_length, log, debug));
}

const std::vector<gr_complex> ofdm_sync_long_impl::LONG = {
gr_complex( -0.04097-0.9626011), gr_complex( 0.3179976-0.8892635), gr_complex( 0.7746551+0.6623833),
gr_complex( 0.1688942+0.2230874), gr_complex( 0.4785908-0.7016541), gr_complex( -0.9210497-0.441444),
gr_complex( -0.3065277-0.8493673), gr_complex( 0.7803301-0.2071068), gr_complex( 0.4267019+0.0326106),
gr_complex( 0.0079118-0.9200371), gr_complex( -1.094439-0.379038), gr_complex( 0.1958068-0.4682544),
gr_complex( 0.4693501-0.119512), gr_complex( -0.179866+1.285259), gr_complex( 0.9539127-0.0327648),
gr_complex( 0.5-0.5), gr_complex( 0.2953435+0.7867532), gr_complex( -0.4576508+0.3143887),
gr_complex( -1.050101+0.521818), gr_complex( 0.6577466+0.7388524), gr_complex( 0.5564548+0.1129757),
gr_complex( -0.4824808+0.650289), gr_complex( -0.451641-0.1744314), gr_complex( -0.28033-1.207107),
gr_complex( -0.9750961-0.1325297), gr_complex( -1.018595-0.164011), gr_complex( 0.6005896-0.5923234),
gr_complex( -0.0224476+0.4301941), gr_complex( -0.7351004+0.9210297), gr_complex( 0.7337324+0.8469733),
gr_complex( 0.0982767+0.7807964), gr_complex( -1.25+0), gr_complex( 0.0982767-0.7807964),
gr_complex( 0.7337324-0.8469733), gr_complex( -0.7351004-0.9210297), gr_complex( -0.0224476-0.4301941),
gr_complex( 0.6005896+0.5923234), gr_complex( -1.018595+0.164011), gr_complex( -0.9750961+0.1325297),
gr_complex( -0.28033+1.207107), gr_complex( -0.451641+0.1744314), gr_complex( -0.4824808-0.650289),
gr_complex( 0.5564548-0.1129757), gr_complex( 0.6577466-0.7388524), gr_complex( -1.050101-0.521818),
gr_complex( -0.4576508-0.3143887), gr_complex( 0.2953435-0.7867532), gr_complex( 0.5+0.5),
gr_complex( 0.9539127+0.0327648), gr_complex( -0.179866-1.285259), gr_complex( 0.4693501+0.119512),
gr_complex( 0.1958068+0.4682544), gr_complex( -1.094439+0.379038), gr_complex( 0.0079118+0.9200371),
gr_complex( 0.4267019-0.0326106), gr_complex( 0.7803301+0.2071068), gr_complex( -0.3065277+0.8493673),
gr_complex( -0.9210497+0.441444), gr_complex( 0.4785908+0.7016541), gr_complex( 0.1688942-0.2230874),
gr_complex( 0.7746551-0.6623833), gr_complex( 0.3179976+0.8892635), gr_complex( -0.04097+0.9626011),
gr_complex( 1.25+0)};
