
#include "allrefs.h"
#include "logging.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <fstream>
#include <iostream>
#include <locale>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>

bool RefRecords::saveToFile(const boost::filesystem::path & dataref_filename, const boost::filesystem::path & commandref_filename) const {
	std::vector<const std::string *> dataref_names, commandref_names;
	dataref_names.reserve(datarefs.size());
	for(const DataRefRecord & dr : datarefs) {
		dataref_names.push_back(&dr.getName());
	}

	commandref_names.reserve(commandrefs.size());
	for(const CommandRefRecord & cr : commandrefs) {
		commandref_names.push_back(&cr.getName());
	}

	auto sort_and_write_names = [](std::vector<const std::string *> & names, const boost::filesystem::path & filename) -> bool {
		auto p_str_comparator = [](const std::string * s1, const std::string * s2) -> bool {
			return boost::ilexicographical_compare(*s1, *s2);
		};
		std::sort(names.begin(), names.end(), p_str_comparator);

		std::ofstream f(filename.string());
		for(const std::string * pstr : names) {
			f << *pstr << "\n";
		}
		f.close();
		return f.fail();
	};

	return sort_and_write_names(dataref_names, dataref_filename) && sort_and_write_names(commandref_names, commandref_filename);
}

int RefRecords::add(const std::vector<std::string> & names, ref_src_t source) {
	int ok = names.size();
	std::string name;
	for(const std::string & name_untrimmed : names) {
		name = name_untrimmed;
		boost::algorithm::trim(name);

		//check for duplicates:
        NameMapType::iterator existing_location = refs_ordered.find(name);
        if(refs_ordered.cend() != existing_location) {
			continue;
		}

		XPLMDataRef dr = XPLMFindDataRef(name.c_str());
		XPLMCommandRef cr = XPLMFindCommand(name.c_str());
        if(nullptr != dr) {
        	datarefs.emplace_back(name, dr, source);
			ref_pointers.emplace_back(&datarefs.back());
       		refs_ordered.insert(std::make_pair(name, ref_pointers.cend() - 1));
		}

		if(nullptr != cr) {
        	commandrefs.emplace_back(name, cr, source);
			ref_pointers.emplace_back(&commandrefs.back());
       		refs_ordered.insert(std::make_pair(name, ref_pointers.cend() - 1));
		}

		if(nullptr == cr && nullptr == dr) {
			ok--;
		}
	}

	{
		char system_path_c[1000];
		XPLMGetSystemPath(system_path_c);
		boost::filesystem::path system_path(system_path_c);
		boost::filesystem::path output_dir = system_path / "Output" / "preferences";

		saveToFile(output_dir / "drt_last_run_datarefs.txt", output_dir / "drt_last_run_commandrefs.txt");
	}

	return ok;
}
void RefRecords::update() {
	std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	for(DataRefRecord & dr : datarefs) {
        if(false == dr.isBlacklisted()) {
            dr.update(now);
        }
	}
}

std::vector<RefRecord *> RefRecords::search(const std::string & search_term, bool regex, bool case_insensitive, bool changed_recently, bool only_big_changes, bool include_drs, bool include_crs) {

	std::vector<std::regex> search_regexes;
    

	std::vector<std::string> search_terms;
    boost::split(search_terms, search_term, boost::is_any_of(" "));

    //erase empty search terms
    auto end_it = std::remove_if(search_terms.begin(), search_terms.end(), [](const std::string & s)-> bool { return s.empty(); });
    search_terms.erase(end_it, search_terms.end());

	if(regex) {
		search_regexes.reserve(search_terms.size());
		for(const std::string & st : search_terms) {
			try {
				search_regexes.emplace_back(st, std::regex::ECMAScript | std::regex::optimize | (case_insensitive ? std::regex::icase : std::regex::flag_type(0)));
			} catch (std::exception &) {
				std::cerr << "Search expression \"" << st << "\" isn't a valid regex." << std::endl;
                return {};
			}
		}
	}

	// Feels a bit messy using all these lambdas, and also I'm a bit skeptical if it all getting optimized well
	const auto string_search = [case_insensitive](const std::string & haystack, const std::vector<std::string> & needles) -> bool {
		if(case_insensitive) {
			const auto case_insensitive_comparator = [](const char ch1, const char ch2) -> bool { return ::toupper(ch1) == ::toupper(ch2); };
			const auto case_insensitive_single_search = [&case_insensitive_comparator, &haystack](const std::string & needle) -> bool {
				return haystack.cend() != std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), case_insensitive_comparator);
			};
			return std::all_of(needles.cbegin(), needles.cend(), case_insensitive_single_search);
		} else {
			const auto case_sensitive_single_search = [&haystack](const std::string & needle) -> bool {
				return haystack.cend() != std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end());
			};
			return std::all_of(needles.cbegin(), needles.cend(), case_sensitive_single_search);
		}
	};

	const auto regex_search = [](const std::string & haystack, const std::vector<std::regex> & regexes) -> bool {
		auto regex_single_search = [&haystack](const std::regex & r) -> bool {
			return std::regex_search(haystack.cbegin(), haystack.cend(), r);
		};
		return std::all_of(regexes.cbegin(), regexes.cend(), regex_single_search);
	};

    std::vector<RefRecord *> data_out;
	std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	auto search_vector = [&data_out, &search_terms, regex, &search_regexes, &regex_search, &string_search, changed_recently, only_big_changes, now](auto & vec) -> void {
		for(auto & record : vec) {
			// Deal with update time first, as this will eliminate most regexes / string compares when it can
			if(changed_recently) {
				float timediff;
				if(only_big_changes) {
					timediff = float(std::chrono::duration_cast<std::chrono::seconds>(now - record.getLastBigUpdateTime()).count());
				} else {
					timediff = float(std::chrono::duration_cast<std::chrono::seconds>(now - record.getLastUpdateTime()).count());
				}
				if(timediff > 10.f) {
					continue;
				}
			}

			const std::string & haystack = record.getName();

			//first deal with search term
			if(false == search_terms.empty()) {
				if(regex) {
					if(false == regex_search(haystack, search_regexes)) {
						continue;
					}
				} else {
					if(false == string_search(haystack, search_terms)) {
						continue;
					}
				}
			}

			data_out.push_back(&record);
		}
	};

	//actually perform the search
	if(include_drs) {
		search_vector(datarefs);
	}

	if(include_crs) {
		search_vector(commandrefs);
	}
    
    //sort results
    {
        auto dr_name_sort = [](const RefRecord * a, const RefRecord * b) -> bool {
            return a->getName() < b->getName();
        };
        std::sort(data_out.begin(), data_out.end(), dr_name_sort);
    }
    
    {
        std::string count_message = "Search found " + std::to_string(data_out.size()) + " results";
        LOG(count_message);
    }
    
    return data_out;
}
