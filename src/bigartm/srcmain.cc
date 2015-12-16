#include <stdlib.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <vector>
#include <set>
#include <fstream>

#include "boost/lexical_cast.hpp"
#include "boost/tokenizer.hpp"
#include "boost/algorithm/string.hpp"
#include "boost/algorithm/string/predicate.hpp"
#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"

#include "boost/filesystem.hpp"
namespace fs = boost::filesystem;

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include "artm/cpp_interface.h"
#include "artm/messages.pb.h"
#include "glog/logging.h"
using namespace artm;

class CuckooWatch {
 public:
  explicit CuckooWatch(std::string message)
    : message_(message), start_(std::chrono::system_clock::now()) {}
  ~CuckooWatch() {
    auto delta = (std::chrono::system_clock::now() - start_);
    auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
    std::cerr << message_ << " " << delta_ms.count() << " milliseconds.\n";
  }

 private:
  std::string message_;
  std::chrono::time_point<std::chrono::system_clock> start_;
};

std::vector<std::string> findFilesInDirectory(std::string root, std::string ext) {
  std::vector<std::string> retval;
  if (boost::filesystem::exists(root) && boost::filesystem::is_directory(root)) {
    boost::filesystem::recursive_directory_iterator it(root);
    boost::filesystem::recursive_directory_iterator endit;
    while (it != endit) {
      if (boost::filesystem::is_regular_file(*it) && it->path().extension() == ext) {
        retval.push_back(it->path().string());
      }
      ++it;
    }
  }
  return retval;
}

class CsvEscape {
 private:
  char delimiter_;

 public:
  explicit CsvEscape(char delimiter) : delimiter_(delimiter) {}

  std::string apply(const std::string& in) {
    if (delimiter_ == '\0')
      return in;

    if (in.find(delimiter_) == std::string::npos)
      return in;

    std::stringstream ss;
    ss << "\"";
    for (unsigned i = 0; i < in.size(); ++i) {
      if (in[i] == '"') ss << "\"\"";
      else ss << in[i];
    }
    ss << "\"";

    return ss.str();
  }
};

class ProgressScope {
 public:
   explicit ProgressScope(const std::string& message)  {
     std::cerr << message << "... ";
   }

   ~ProgressScope() {
     if (std::uncaught_exception()) {
       std::cerr << "Failed\n";
     } else {
       std::cerr << "OK.\n";
     }
   }
};

bool parseNumberOrPercent(std::string str, double* value, bool* fraction ) {
  if (str.empty())
    return false;

  bool percent = false;
  if (str[str.size() - 1] == '%') {
    percent = true;
    str = str.substr(0, str.size() - 1);
  }

  *value = 0;
  *fraction = true;
  try {
    *value = boost::lexical_cast<double>(str);
  }
  catch (...) {
    return false;
  }

  if (percent) {
    *value /= 100;
    return true;
  }

  if (*value >= 1.0) {
    *fraction = false;
  }

  return true;
}

template<typename T>
std::vector<std::pair<std::string, T>> parseKeyValuePairs(const std::string& input) {
  std::vector<std::pair<std::string, T>> retval;
  try {
    // Handle the case when "input" simply has an instance of typename T
    T single_value = boost::lexical_cast<T>(input);
    retval.push_back(std::make_pair(std::string(), single_value));
    return retval;
  } catch (...) {}

  // Handle the case when "input" is a set of "group:value" pairs, separated by ; or ,
  std::vector<std::string> strs;
  boost::split(strs, input, boost::is_any_of(";,"));
  for (unsigned elem_index = 0; elem_index < strs.size(); ++elem_index) {
    std::string elem = strs[elem_index];
    T elem_size = 0;
    size_t split_index = elem.find(':');
    if (split_index == 0 || (split_index == elem.size() - 1))
      split_index = std::string::npos;

    if (split_index != std::string::npos) {
      try {
        elem_size = boost::lexical_cast<T>(elem.substr(split_index + 1));
        elem = elem.substr(0, split_index);
      } catch (...) {
        split_index = std::string::npos;
      }
    }

    retval.push_back(std::make_pair(elem, elem_size));
  }

  return retval;
}

std::vector<std::pair<std::string, std::vector<std::string>>> parseTopicGroups(const std::string& topics) {
  std::vector<std::pair<std::string, std::vector<std::string>>> result;
  std::vector<std::pair<std::string, int>> pairs = parseKeyValuePairs<int>(topics);
  for (auto& pair : pairs) {
    const std::string group = pair.first.empty() ? "topic" : pair.first;
    const int group_size = pair.second == 0 ? 1 : pair.second;
    std::vector<std::string> group_list;
    if (group_size == 1) {
      group_list.push_back(group);
    }
    else {
      for (int i = 0; i < group_size; ++i)
        group_list.push_back(group + "_" + boost::lexical_cast<std::string>(i));
    }
    result.push_back(std::make_pair(group, group_list));
  }

  return result;

}

std::vector<std::string> parseTopics(const std::string& topics) {
  std::vector<std::string> result;
  std::vector<std::pair<std::string, std::vector<std::string>>> pairs = parseTopicGroups(topics);
  for (auto& pair : pairs)
    for (auto& topic_name : pair.second)
      result.push_back(topic_name);

  return result;
}

std::vector<std::string> parseTopics(const std::string& topics, const std::string& topic_groups) {
  std::vector<std::string> result;

  std::vector<std::pair<std::string, std::vector<std::string>>> pairs = parseTopicGroups(topic_groups);
  std::vector<std::string> topic_names = parseTopics(topics);
  for (auto& topic_name : topic_names) {
    bool found = false;
    for (auto& pair : pairs) {
      if (pair.first == topic_name) {
        for (auto& group_topic : pair.second)
          result.push_back(group_topic);
        found = true;
        break;
      }
    }

    if (!found)
      result.push_back(topic_name);
  }

  return result;
}

struct artm_options {
  // Corpus / batches
  std::string read_uci_docword;
  std::string read_uci_vocab;
  std::string read_vw_corpus;
  std::string read_cooc;
  std::string use_batches;
  int batch_size;

  // Dictionary
  std::string use_dictionary;
  std::string dictionary_min_df;
  std::string dictionary_max_df;

  // Model
  std::string load_model;
  std::string topics;
  std::string use_modality;
  std::string predict_class;

  // Learning
  int passes;
  int inner_iterations_count;
  int update_every;
  float tau0;
  float kappa;
  std::vector<std::string> regularizer;
  bool b_reuse_theta;
  int threads;
  bool async;

  // Output
  std::string save_model;
  std::string save_dictionary;
  std::string save_batches;
  std::string write_model_readable;
  std::string write_predictions;
  std::string write_class_predictions;
  std::string csv_separator;
  int score_level;
  std::vector<std::string> score;
  std::vector<std::string> final_score;
  std::string pwt_model_name;
  std::string main_dictionary_name;

  // Other options
  std::string disk_cache_folder;
  std::string response_file;
  bool b_paused;
  bool b_disable_avx_opt;
  bool b_use_dense_bow;

  artm_options() {
    pwt_model_name = "pwt";
    main_dictionary_name = "main_dictionary";
  }
};

void fixOptions(artm_options* options) {
  if (boost::to_lower_copy(options->csv_separator) == "tab")
    options->csv_separator = "\t";
}

void fixScoreLevel(artm_options* options) {
  if (!options->score.empty() || !options->final_score.empty()) {
    options->score_level = 0;
    return;
  }

  std::vector<std::pair<std::string, float>> class_ids_map = parseKeyValuePairs<float>(options->use_modality);
  std::vector<std::string> class_ids;

  for (auto& class_id : class_ids_map) {
    if (!class_id.first.empty()) {
      std::stringstream ss;
      ss << " @" << class_id.first;
      class_ids.push_back(ss.str());
    } else {
      class_ids.push_back(std::string());
    }
  }

  if (class_ids.empty())
    class_ids.push_back(std::string());

  if (options->score_level >= 1) {
    options->score.push_back("Perplexity");
    for (auto& class_id : class_ids)
      options->score.push_back(std::string("SparsityPhi") + class_id);
    options->score.push_back("SparsityTheta");
    if (!options->predict_class.empty())
      options->score.push_back("ClassPrecision");
  }

  if (options->score_level >= 2) {
    for (auto& class_id : class_ids)
      options->final_score.push_back(std::string("TopTokens") + class_id);
    options->final_score.push_back("ThetaSnippet");
  }

  if (options->score_level >= 3) {
    options->score.push_back("TopicKernel");
  }
}

std::string addToDictionaryMap(std::map<std::string, std::string>* dictionary_map, std::string dictionary_path) {
  if (dictionary_path.empty())
    return std::string();

  auto mapped_name = dictionary_map->find(dictionary_path);
  if (mapped_name == dictionary_map->end()) {
    dictionary_map->insert(std::make_pair(
      dictionary_path,
      boost::lexical_cast<std::string>(boost::uuids::random_generator()())));
    mapped_name = dictionary_map->find(dictionary_path);
  }

  return mapped_name->second;
}

void configureRegularizer(const std::string& regularizer, const std::string& topics,
                          std::map<std::string, std::string>* dictionary_map,
                          RegularizeModelArgs* regularize_model_args,
                          ProcessBatchesArgs* process_batches_args, artm::RegularizerConfig* config) {
  std::vector<std::string> strs;
  boost::split(strs, regularizer, boost::is_any_of("\t "));
  if (strs.size() < 2)
    throw std::invalid_argument(std::string("Invalid regularizer: " + regularizer));
  float tau;
  try {
    tau = boost::lexical_cast<float>(strs[0]);
  } catch (...) {
    throw std::invalid_argument(std::string("Invalid regularizer: " + regularizer));
  }

  std::vector<std::pair<std::string, float>> class_ids;
  std::vector<std::string> topic_names;
  std::string dictionary_path;
  for (unsigned i = 2; i < strs.size(); ++i) {
    std::string elem = strs[i];
    if (elem.empty())
      continue;
    if (elem[0] == '#')
      topic_names = parseTopics(elem.substr(1, elem.size() - 1), topics);
    else if (elem[0] == '@')
      class_ids = parseKeyValuePairs<float>(elem.substr(1, elem.size() - 1));
    else if (elem[0] == '!')
      dictionary_path = elem.substr(1, elem.size() - 1);
  }

  std::string dictionary_name = addToDictionaryMap(dictionary_map, dictionary_path);

  // SmoothPhi, SparsePhi, SmoothTheta, SparseTheta, Decorrelation
  std::string regularizer_type = boost::to_lower_copy(strs[1]);
  if (regularizer_type == "smooththeta" || regularizer_type == "sparsetheta") {
    ::artm::SmoothSparseThetaConfig specific_config;
    for (auto& topic_name : topic_names)
      specific_config.add_topic_name(topic_name);

    if (regularizer_type == "sparsetheta") tau = -tau;

    config->set_name(regularizer);
    config->set_type(::artm::RegularizerConfig_Type_SmoothSparseTheta);
    config->set_config(specific_config.SerializeAsString());

    process_batches_args->add_regularizer_name(regularizer);
    process_batches_args->add_regularizer_tau(tau);
  }
  else if (regularizer_type == "smoothphi" || regularizer_type == "sparsephi") {
    ::artm::SmoothSparsePhiConfig specific_config;
    for (auto& topic_name : topic_names)
      specific_config.add_topic_name(topic_name);
    for (auto& class_id : class_ids)
      specific_config.add_class_id(class_id.first);
    if (!dictionary_name.empty())
      specific_config.set_dictionary_name(dictionary_name);

    if (regularizer_type == "sparsephi") tau = -tau;

    config->set_name(regularizer);
    config->set_type(::artm::RegularizerConfig_Type_SmoothSparsePhi);
    config->set_config(specific_config.SerializeAsString());

    artm::RegularizerSettings* settings = regularize_model_args->add_regularizer_settings();
    settings->set_name(regularizer);
    settings->set_tau(tau);
    settings->set_use_relative_regularization(false);
  }
  else if (regularizer_type == "decorrelation") {
    ::artm::DecorrelatorPhiConfig specific_config;
    for (auto& topic_name : topic_names)
      specific_config.add_topic_name(topic_name);
    for (auto& class_id : class_ids)
      specific_config.add_class_id(class_id.first);

    config->set_name(regularizer);
    config->set_type(::artm::RegularizerConfig_Type_DecorrelatorPhi);
    config->set_config(specific_config.SerializeAsString());

    artm::RegularizerSettings* settings = regularize_model_args->add_regularizer_settings();
    settings->set_name(regularizer);
    settings->set_tau(tau);
    settings->set_use_relative_regularization(false);
  } else {
    throw std::invalid_argument(std::string("Unknown regularizer type: " + strs[1]));
  }
}

class ScoreHelper {
 private:
   ::artm::MasterComponent* master_;
   std::map<std::string, std::string>* dictionary_map_;
   std::vector<std::pair<std::string, ::artm::ScoreConfig_Type>> score_name_;
 public:
   ScoreHelper(::artm::MasterComponent* master, std::map<std::string, std::string>* dictionary_map)
       : master_(master), dictionary_map_(dictionary_map) {}

   void addScore(const std::string& score, const std::string& topics) {
     std::vector<std::string> strs;
     boost::split(strs, score, boost::is_any_of("\t "));
     if (strs.size() < 1)
       throw std::invalid_argument(std::string("Invalid score: " + score));

     std::vector<std::pair<std::string, float>> class_ids;
     std::vector<std::string> topic_names;
     std::string dictionary_path;
     for (unsigned i = 1; i < strs.size(); ++i) {
       std::string elem = strs[i];
       if (elem.empty())
         continue;
       if (elem[0] == '#')
         topic_names = parseTopics(elem.substr(1, elem.size() - 1), topics);
       else if (elem[0] == '@')
         class_ids = parseKeyValuePairs<float>(elem.substr(1, elem.size() - 1));
       else if (elem[0] == '!')
         dictionary_path = elem.substr(1, elem.size() - 1);
     }

     std::string dictionary_name = addToDictionaryMap(dictionary_map_, dictionary_path);

     // Perplexity,SparsityTheta,SparsityPhi,TopTokens,ThetaSnippet,TopicKernel
     std::string score_type = boost::to_lower_copy(strs[0]);
     size_t langle = score_type.find('(');
     size_t rangle = score_type.find(')');
     float score_arg = 0;
     if (langle != std::string::npos && rangle != std::string::npos && (rangle - langle) >= 2) {
       try {
         score_arg = boost::lexical_cast<float>(score_type.substr(langle + 1, rangle - langle - 1));
         score_type = score_type.substr(0, langle);
       }
       catch (...) {}
     }

     ::artm::ScoreConfig score_config;
     std::shared_ptr< ::google::protobuf::Message> specific_config;
     score_config.set_name(score);
     if (score_type == "perplexity") {
       PerplexityScoreConfig specific_config;
       for (auto& class_id : class_ids) specific_config.add_class_id(class_id.first);
       if (dictionary_name.empty()) {
         specific_config.set_model_type(PerplexityScoreConfig_Type_UnigramDocumentModel);
       } else {
         specific_config.set_model_type(PerplexityScoreConfig_Type_UnigramCollectionModel);
         specific_config.set_dictionary_name(dictionary_name);
       }
       score_config.set_type(::artm::ScoreConfig_Type_Perplexity);
       score_config.set_config(specific_config.SerializeAsString());
     }
     else if (score_type == "sparsitytheta") {
       SparsityThetaScoreConfig specific_config;
       for (auto& topic_name : topic_names) specific_config.add_topic_name(topic_name);
       score_config.set_type(::artm::ScoreConfig_Type_SparsityTheta);
       score_config.set_config(specific_config.SerializeAsString());
     }
     else if (score_type == "sparsityphi") {
       SparsityPhiScoreConfig specific_config;
       for (auto& topic_name : topic_names) specific_config.add_topic_name(topic_name);
       for (auto& class_id : class_ids) specific_config.set_class_id(class_id.first);
       score_config.set_type(::artm::ScoreConfig_Type_SparsityPhi);
       score_config.set_config(specific_config.SerializeAsString());
     }
     else if (score_type == "toptokens") {
       TopTokensScoreConfig specific_config;
       if (score_arg != 0) specific_config.set_num_tokens(static_cast<int>(score_arg));
       for (auto& topic_name : topic_names) specific_config.add_topic_name(topic_name);
       for (auto& class_id : class_ids) specific_config.set_class_id(class_id.first);
       if (!dictionary_name.empty()) specific_config.set_cooccurrence_dictionary_name(dictionary_name);
       score_config.set_type(::artm::ScoreConfig_Type_TopTokens);
       score_config.set_config(specific_config.SerializeAsString());
     }
     else if (score_type == "thetasnippet") {
       ThetaSnippetScoreConfig specific_config;
       if (score_arg != 0) specific_config.set_item_count(score_arg);
       score_config.set_type(::artm::ScoreConfig_Type_ThetaSnippet);
       score_config.set_config(specific_config.SerializeAsString());
     }
     else if (score_type == "topickernel") {
       TopicKernelScoreConfig specific_config;
       if (score_arg != 0) specific_config.set_probability_mass_threshold(score_arg);
       for (auto& topic_name : topic_names) specific_config.add_topic_name(topic_name);
       for (auto& class_id : class_ids) specific_config.set_class_id(class_id.first);
       if (!dictionary_name.empty()) specific_config.set_cooccurrence_dictionary_name(dictionary_name);
       score_config.set_type(::artm::ScoreConfig_Type_TopicKernel);
       score_config.set_config(specific_config.SerializeAsString());
     }
     else if (score_type == "classprecision") {
       ClassPrecisionScoreConfig specific_config;
       score_config.set_type(::artm::ScoreConfig_Type_ClassPrecision);
       score_config.set_config(specific_config.SerializeAsString());
     }
     else {
       throw std::invalid_argument(std::string("Unknown regularizer type: " + strs[0]));
     }
     master_->mutable_config()->add_score_config()->CopyFrom(score_config);
     master_->Reconfigure(master_->config());
     score_name_.push_back(std::make_pair(score, score_config.type()));
   }

   void showScore(const std::string model_name, std::string& score_name, ::artm::ScoreConfig_Type type) {
     if (type == ::artm::ScoreConfig_Type_Perplexity) {
       auto score_data = master_->GetScoreAs< ::artm::PerplexityScore>(model_name, score_name);
       std::cerr << "Perplexity      = " << score_data->value();
       if (boost::to_lower_copy(score_name) != "perplexity") std::cerr << "\t(" << score_name << ")";
       std::cerr << "\n";
     }
     else if (type == ::artm::ScoreConfig_Type_SparsityTheta) {
       auto score_data = master_->GetScoreAs< ::artm::SparsityThetaScore>(model_name, score_name);
       std::cerr << "SparsityTheta   = " << score_data->value();
       if (boost::to_lower_copy(score_name) != "sparsitytheta") std::cerr << "\t(" << score_name << ")";
       std::cerr << "\n";
     }
     else if (type == ::artm::ScoreConfig_Type_SparsityPhi) {
       auto score_data = master_->GetScoreAs< ::artm::SparsityPhiScore>(model_name, score_name);
       std::cerr << "SparsityPhi     = " << score_data->value();
       if (boost::to_lower_copy(score_name) != "sparsityphi") std::cerr << "\t(" << score_name << ")";
       std::cerr << "\n";
     }
     else if (type == ::artm::ScoreConfig_Type_TopTokens) {
       auto score_data = master_->GetScoreAs< ::artm::TopTokensScore>(model_name, score_name);
       std::cerr << "TopTokens (" << score_name << "):";
       int topic_index = -1;
       for (int i = 0; i < score_data->num_entries(); i++) {
         if (score_data->topic_index(i) != topic_index) {
           topic_index = score_data->topic_index(i);
           std::cerr << "\n#" << (topic_index + 1) << ": ";
         }

         std::cerr << score_data->token(i) << "(" << std::setw(2) << std::setprecision(2) << score_data->weight(i) << ") ";
       }
       std::cerr << "\n";
     }
     else if (type == ::artm::ScoreConfig_Type_ThetaSnippet) {
       auto score_data = master_->GetScoreAs< ::artm::ThetaSnippetScore>(model_name, score_name);
       int docs_to_show = score_data->values_size();
       std::cerr << "ThetaSnippet (" << score_name << ")\n";
       for (int item_index = 0; item_index < score_data->values_size(); ++item_index) {
         std::cerr << "ItemID=" << score_data->item_id(item_index) << ": ";
         const FloatArray& values = score_data->values(item_index);
         for (int topic_index = 0; topic_index < values.value_size(); ++topic_index) {
           float weight = values.value(topic_index);
           std::cerr << std::fixed << std::setw(4) << std::setprecision(5) << weight << " ";
         }
         std::cerr << "\n";
       }
     }
     else if (type == ::artm::ScoreConfig_Type_TopicKernel) {
       auto score_data = master_->GetScoreAs< ::artm::TopicKernelScore>(model_name, score_name);
       std::stringstream suffix;
       if (boost::to_lower_copy(score_name) != "topickernel") suffix << "\t(" << score_name << ")";

       std::cerr << "KernelSize      = " << score_data->average_kernel_size() << suffix.str() << "\n";
       std::cerr << "KernelPurity    = " << score_data->average_kernel_purity() << suffix.str() << "\n";
       std::cerr << "KernelContrast  = " << score_data->average_kernel_contrast() << suffix.str() << "\n";
       if (score_data->has_average_coherence())
         std::cerr << "KernelCoherence = " << score_data->average_coherence() << suffix.str() << "\n";
     }
     else if (type == ::artm::ScoreConfig_Type_ClassPrecision) {
       auto score_data = master_->GetScoreAs< ::artm::ClassPrecisionScore>(model_name, score_name);
       std::stringstream suffix;
       if (boost::to_lower_copy(score_name) != "classprecision") suffix << "\t(" << score_name << ")";
       std::cerr << "ClassPrecision  = " << score_data->value() << suffix.str() << "\n";
     }
     else {
       throw std::invalid_argument("Unknown score config type: " + boost::lexical_cast<std::string>(type));
     }
   }

   void showScores(const std::string model_name) {
     for (auto& score_name : score_name_)
       showScore(model_name, score_name.first, score_name.second);
   }
};

class BatchVectorizer {
 private:
  std::string batch_folder_;
  const artm_options& options_;
  std::string cleanup_folder_;

 public:
  BatchVectorizer(const artm_options& options) : batch_folder_(), options_(options), cleanup_folder_() {}

  void Vectorize() {
    const bool parse_vw_format = !options_.read_vw_corpus.empty();
    const bool parse_uci_format = !options_.read_uci_docword.empty();
    const bool use_batches = !options_.use_batches.empty();

    if (((int)parse_vw_format + (int)parse_uci_format + (int)use_batches) >= 2)
      throw std::invalid_argument("--read_vw_format, --read-uci-docword, --use-batches must not be used together");
    if (parse_uci_format && options_.read_uci_vocab.empty())
      throw std::invalid_argument("--read-uci-vocab option must be specified together with --read-uci-docword\n");

    if (parse_vw_format || parse_uci_format) {
      if (options_.save_batches.empty()) {
        batch_folder_ = boost::lexical_cast<std::string>(boost::uuids::random_generator()());
        cleanup_folder_ = batch_folder_;
      }
      else {
        batch_folder_ = options_.save_batches;
      }

      if (fs::exists(fs::path(batch_folder_)) && !fs::is_empty(fs::path(batch_folder_)))
        std::cerr << "Warning: --save-batches folder already exists, new batches will be added into " << batch_folder_ << "\n";

      boost::system::error_code error;
      fs::create_directories(batch_folder_, error);
      if (error)
        throw std::runtime_error(std::string("Unable to create batch folder: ") + batch_folder_);

      ProgressScope scope("Parsing text collection");
      ::artm::CollectionParserConfig collection_parser_config;
      if (parse_uci_format) collection_parser_config.set_format(CollectionParserConfig_Format_BagOfWordsUci);
      else if (parse_vw_format) collection_parser_config.set_format(CollectionParserConfig_Format_VowpalWabbit);
      else throw std::runtime_error("Internal error in bigartm.exe - unable to determine CollectionParserConfig_Format");

      collection_parser_config.set_docword_file_path(parse_vw_format ? options_.read_vw_corpus : options_.read_uci_docword);
      if (!options_.read_uci_vocab.empty())
        collection_parser_config.set_vocab_file_path(options_.read_uci_vocab);
      collection_parser_config.set_target_folder(batch_folder_);
      collection_parser_config.set_num_items_per_batch(options_.batch_size);
      ::artm::ParseCollection(collection_parser_config);
    }
    else {
      batch_folder_ = options_.use_batches;
      if (!fs::exists(fs::path(batch_folder_)))
        throw std::runtime_error(std::string("Unable to find batch folder: ") + batch_folder_);

      int batch_files_count = findFilesInDirectory(batch_folder_, ".batch").size();
      if (batch_files_count == 0)
        throw std::runtime_error(std::string("No batches found in batch folder: ") + batch_folder_);

      std::cerr << "Using " << batch_files_count << " batches from '" << batch_folder_ << "'\n";
    }
  }

  ~BatchVectorizer() {
    if (options_.save_batches.empty()) {
      try { boost::filesystem::remove_all(cleanup_folder_); }
      catch (...) {}
    }
  }

  const std::string& batch_folder() { return batch_folder_; }
};

class BatchesIterator {
 private:
  std::vector<std::string> batch_file_names_;
  int update_every_;
  unsigned current_;

 public:
  BatchesIterator(const std::vector<std::string>& batch_file_names, int update_every)
      : batch_file_names_(batch_file_names), update_every_(update_every), current_(0) {}

  bool more() { return current_ < batch_file_names_.size(); }

  void get(ProcessBatchesArgs* args) {
    args->clear_batch_filename();
    unsigned last = std::min<unsigned>(current_ + update_every_, batch_file_names_.size());
    if (update_every_ <= 0) last = batch_file_names_.size();  // offline algorighm
    for (; current_ < last; current_++)
      args->add_batch_filename(batch_file_names_[current_]);
  }

  void rewind() { current_ = 0; }
};

class StringIndex {
public:
  StringIndex(std::string prefix) : i_(0), prefix_(prefix) {}
  StringIndex(std::string prefix, int i) : i_(i), prefix_(prefix) {}
  int getI() { return i_; }
  operator std::string() const { return prefix_ + boost::lexical_cast<std::string>(i_); }
  StringIndex operator+(int offset) { return StringIndex(prefix_, i_ + offset); }
  StringIndex operator-(int offset) { return StringIndex(prefix_, i_ - offset); }
  int operator++() { return ++i_; }
  int operator++(int) { return i_++; }

private:
  int i_;
  std::string prefix_;
};

class ArtmExecutor {
 public:
  ArtmExecutor(const artm_options& options,
               const std::vector<std::string>& batches_file_name,
               MasterComponent* master_component,
               ProcessBatchesArgs* process_batches_args,
               RegularizeModelArgs* regularize_model_args)
      : options_(options),
        batch_iterator_(batches_file_name, options.update_every),
        master_component_(master_component),
        process_batches_args_(process_batches_args),
        regularize_model_args_(regularize_model_args) {}

  void ExecuteOfflineAlgorithm() {
    const std::string nwt_hat_model_name = "nwt_hat";
    const std::string rwt_model_name = "rwt";

    ProcessBatches(options_.pwt_model_name, nwt_hat_model_name);
    Regularize(options_.pwt_model_name, nwt_hat_model_name, rwt_model_name);
    Normalize(options_.pwt_model_name, nwt_hat_model_name, rwt_model_name);
  }

  void ExecuteOnlineAlgorithm(int* update_count) {
    const std::string nwt_hat_model_name = "nwt_hat";
    const std::string rwt_model_name = "rwt";
    const std::string nwt_model_name = "nwt";

    process_batches_args_->set_reset_scores(true);  // reset scores at the beginning of each iteration
    while (batch_iterator_.more()) {
      ProcessBatches(options_.pwt_model_name, nwt_hat_model_name, update_count);

      double apply_weight = (*update_count == 1) ? 1.0 : pow(options_.tau0 + (*update_count), -options_.kappa);
      double decay_weight = 1.0 - apply_weight;

      Merge(nwt_model_name, decay_weight, nwt_hat_model_name, apply_weight);
      Regularize(options_.pwt_model_name, nwt_model_name, rwt_model_name);
      Normalize(options_.pwt_model_name, nwt_model_name, rwt_model_name);

      process_batches_args_->set_reset_scores(false);
    }  // while (batch_source.more())
  }

  void ExecuteAsyncOnlineAlgorithm(int* update_count) {
    /**************************************************
    1. Enough batches.
    i = 0: process(b1, pwt,  nwt0)
    i = 1: process(b2, pwt,  nwt1) wait(nwt0) merge(nwt, nwt0) dispose(nwt0) regularize(pwt,  nwt, rwt) normalize(nwt, rwt, pwt2) dispose(pwt0)
    i = 2: process(b3, pwt2, nwt2) wait(nwt1) merge(nwt, nwt1) dispose(nwt1) regularize(pwt2, nwt, rwt) normalize(nwt, rwt, pwt3) dispose(pwt1)
    i = 3: process(b4, pwt3, nwt3) wait(nwt2) merge(nwt, nwt2) dispose(nwt2) regularize(pwt3, nwt, rwt) normalize(nwt, rwt, pwt4) dispose(pwt2)
    i = 4: process(b5, pwt4, nwt4) wait(nwt3) merge(nwt, nwt3) dispose(nwt3) regularize(pwt4, nwt, rwt) normalize(nwt, rwt, pwt5) dispose(pwt3)
    i = 4:                         wait(nwt4) merge(nwt, nwt4) dispose(nwt4) regularize(pwt5, nwt, rwt) normalize(nwt, rwt, pwt)  dispose(pwt4) dispose(pwt5)

    2. Not enough batches -- same code works just fine.
    i = 0: process(b1, pwt,  nwt0)
    i = 1:                         wait(nwt0) merge(nwt, nwt0) dispose(nwt0) regularize(pwt,  nwt, rwt) normalize(nwt, rwt, pwt)  dispose(pwt0) dispose(pwt1)
    **************************************************/

    std::string pwt_active = options_.pwt_model_name;
    StringIndex pwt_index("pwt");
    StringIndex nwt_index("nwt");

    process_batches_args_->set_model_name_cache(options_.pwt_model_name);
    process_batches_args_->set_reset_scores(true);  // reset scores at the beginning of each iteration
    int op_id = AsyncProcessBatches(pwt_active, nwt_index, update_count);
    process_batches_args_->set_reset_scores(false);

    while (true) {
      bool is_last = !batch_iterator_.more();
      pwt_index++; nwt_index++;

      double apply_weight = (*update_count == 1) ? 1.0 : pow(options_.tau0 + (*update_count), -options_.kappa);
      double decay_weight = 1.0 - apply_weight;

      int temp_op_id = op_id;
      if (!is_last) op_id = AsyncProcessBatches(pwt_active, nwt_index, update_count);
      Await(temp_op_id);
      Merge("NWT", decay_weight, nwt_index - 1, apply_weight);
      Dispose(nwt_index - 1);
      Regularize(pwt_active, "NWT", "RWT");

      pwt_active = is_last ? options_.pwt_model_name : std::string(pwt_index + 1);
      Normalize(pwt_active, "NWT", "RWT");

      Dispose(pwt_index - 1);
      if (is_last) Dispose(pwt_index);
      if (is_last) break;
    }
  }

 private:
  const artm_options& options_;
  BatchesIterator batch_iterator_;
  MasterComponent* master_component_;
  ProcessBatchesArgs* process_batches_args_;
  RegularizeModelArgs* regularize_model_args_;

  void ProcessBatches(std::string pwt, std::string nwt) {
    int update_count = 0;
    ProcessBatches(pwt, nwt, &update_count);
  }

  void ProcessBatches(std::string pwt, std::string nwt, int* update_count) {
    (*update_count)++;
    process_batches_args_->set_pwt_source_name(pwt);
    process_batches_args_->set_nwt_target_name(nwt);
    batch_iterator_.get(process_batches_args_);
    master_component_->ProcessBatches(*process_batches_args_);
    process_batches_args_->clear_batch_filename();
  }

  int AsyncProcessBatches(std::string pwt, std::string nwt, int* update_count) {
    (*update_count)++;
    process_batches_args_->set_pwt_source_name(pwt);
    process_batches_args_->set_nwt_target_name(nwt);
    process_batches_args_->set_theta_matrix_type(ProcessBatchesArgs_ThetaMatrixType_None);
    batch_iterator_.get(process_batches_args_);
    int operation_id = master_component_->AsyncProcessBatches(*process_batches_args_);
    process_batches_args_->clear_batch_filename();
    return operation_id;
  }

  void Await(int operation_id) {
    master_component_->AwaitOperation(operation_id);
  }

  void Regularize(std::string pwt, std::string nwt, std::string rwt) {
    if (regularize_model_args_->regularizer_settings_size() > 0) {
      regularize_model_args_->set_nwt_source_name(nwt);
      regularize_model_args_->set_pwt_source_name(pwt);
      regularize_model_args_->set_rwt_target_name(rwt);
      master_component_->RegularizeModel(*regularize_model_args_);
    }
  }

  void Normalize(std::string pwt, std::string nwt, std::string rwt) {
    NormalizeModelArgs normalize_model_args;
    if (regularize_model_args_->regularizer_settings_size() > 0)
      normalize_model_args.set_rwt_source_name(rwt);
    normalize_model_args.set_nwt_source_name(nwt);
    normalize_model_args.set_pwt_target_name(pwt);
    master_component_->NormalizeModel(normalize_model_args);
  }

  void Merge(std::string nwt, double decay_weight, std::string nwt_hat, double apply_weight) {
    MergeModelArgs merge_model_args;
    merge_model_args.add_nwt_source_name(nwt);
    merge_model_args.add_source_weight(decay_weight);
    merge_model_args.add_nwt_source_name(nwt_hat);
    merge_model_args.add_source_weight(apply_weight);
    merge_model_args.set_nwt_target_name(nwt);
    master_component_->MergeModel(merge_model_args);
  }

  void Dispose(std::string model_name) {
    master_component_->DisposeModel(model_name);
  }
};

void WritePredictions(const artm_options& options,
                      const ::artm::ThetaMatrix& theta_metadata,
                      const ::artm::Matrix& theta_matrix) {
  ProgressScope scope(std::string("Writing model predictions into ") + options.write_predictions);
  CsvEscape escape(options.csv_separator.size() == 1 ? options.csv_separator[0] : '\0');

  std::ofstream output(options.write_predictions);
  const std::string sep = options.csv_separator;

  // header
  output << "id" << sep << "title";
  for (int j = 0; j < theta_metadata.topics_count(); ++j) {
    if (theta_metadata.topic_name_size() > 0)
      output << sep << escape.apply(theta_metadata.topic_name(j));
    else
      output << sep << "topic" << j;
  }
  output << std::endl;

  std::vector<std::pair<int, int>> id_to_index;
  for (int i = 0; i < theta_metadata.item_id_size(); ++i)
    id_to_index.push_back(std::make_pair(theta_metadata.item_id(i), i));
  std::sort(id_to_index.begin(), id_to_index.end());

  // bulk
  for (int i = 0; i < theta_metadata.item_id_size(); ++i) {
    int index = id_to_index[i].second;
    output << theta_metadata.item_id(index) << sep;
    output << (theta_metadata.item_title_size() == 0 ? "" : escape.apply(theta_metadata.item_title(index)));
    for (int j = 0; j < theta_metadata.topics_count(); ++j) {
      output << sep << theta_matrix(index, j);
    }
    output << std::endl;
  }
}

void WriteClassPredictions(const artm_options& options,
                           const ::artm::ThetaMatrix& theta_metadata,
                           const ::artm::Matrix& theta_matrix) {
  ProgressScope scope(std::string("Writing model class predictions into ") + options.write_class_predictions);
  CsvEscape escape(options.csv_separator.size() == 1 ? options.csv_separator[0] : '\0');

  std::ofstream output(options.write_class_predictions);
  const std::string sep = options.csv_separator;

  // header
  output << "id" << sep << "title" << sep << options.predict_class << std::endl;

  std::vector<std::pair<int, int>> id_to_index;
  for (int i = 0; i < theta_metadata.item_id_size(); ++i)
    id_to_index.push_back(std::make_pair(theta_metadata.item_id(i), i));
  std::sort(id_to_index.begin(), id_to_index.end());

  // bulk
  for (int i = 0; i < theta_metadata.item_id_size(); ++i) {
    int index = id_to_index[i].second;

    float max = 0;
    float max_index = 0;
    for (int j = 0; j < theta_metadata.topics_count(); ++j) {
      float value = theta_matrix(index, j);
      if (value > max) {
        max = value;
        max_index = j;
      }
    }

    output << theta_metadata.item_id(index) << sep;
    output << (theta_metadata.item_title_size() == 0 ? "" : escape.apply(theta_metadata.item_title(index))) << sep;
    output << theta_metadata.topic_name(max_index) << std::endl;
  }
}

int execute(const artm_options& options) {
  bool online = (options.update_every > 0);

  const std::string dictionary_name = "dictionary";
  const std::string pwt_model_name = options.pwt_model_name;

  if (options.b_paused) {
    std::cerr << "Press any key to continue. ";
    getchar();
  }

  std::vector<std::string> topic_names = parseTopics(options.topics);

  // Step 1. Configuration
  MasterComponentConfig master_config;
  master_config.set_processors_count(options.threads);
  if (options.b_reuse_theta) master_config.set_cache_theta(true);
  if (!options.disk_cache_folder.empty()) master_config.set_disk_cache_path(options.disk_cache_folder);

  ProcessBatchesArgs process_batches_args;
  process_batches_args.set_inner_iterations_count(options.inner_iterations_count);
  process_batches_args.set_opt_for_avx(!options.b_disable_avx_opt);
  process_batches_args.set_use_sparse_bow(!options.b_use_dense_bow);
  if (!options.predict_class.empty()) process_batches_args.set_predict_class_id(options.predict_class);
  if (options.b_reuse_theta) process_batches_args.set_reuse_theta(true);

  std::vector<std::pair<std::string, float>> class_ids = parseKeyValuePairs<float>(options.use_modality);
  for (auto& class_id : class_ids) {
    if (class_id.first.empty())
      continue;
    process_batches_args.add_class_id(class_id.first);
    process_batches_args.add_class_weight(class_id.second == 0.0f ? 1.0f : class_id.second);
  }

  RegularizeModelArgs regularize_model_args;

  // Step 2. Collection parsing
  BatchVectorizer batch_vectorizer(options);
  batch_vectorizer.Vectorize();

  // Step 3. Create master component.
  std::shared_ptr<MasterComponent> master_component;
  master_component.reset(new MasterComponent(master_config));

  // Step 3.1. Parse or import the main dictionary
  if (options.use_dictionary.empty()) {
    ProgressScope scope(std::string("Gathering dictionary from batches"));
    ::artm::GatherDictionaryArgs gather_dictionary_args;
    gather_dictionary_args.set_dictionary_target_name(options.main_dictionary_name);
    gather_dictionary_args.set_data_path(batch_vectorizer.batch_folder());
    if (!options.read_cooc.empty()) gather_dictionary_args.set_cooc_file_path(options.read_cooc);
    if (!options.read_uci_vocab.empty()) gather_dictionary_args.set_vocab_file_path(options.read_uci_vocab);
    master_component->GatherDictionary(gather_dictionary_args);
  } else {
    ProgressScope scope(std::string("Loading dictionary file from ") + options.use_dictionary);
    ImportDictionaryArgs import_dictionary_args;
    import_dictionary_args.set_file_name(options.use_dictionary);
    import_dictionary_args.set_dictionary_name(options.main_dictionary_name);
    master_component->ImportDictionary(import_dictionary_args);
  }

  // Step 3.2. Filter dictionary
  if (!options.dictionary_max_df.empty() || !options.dictionary_min_df.empty()) {
    ProgressScope scope("Filtering dictionary based on user thresholds");
    ::artm::FilterDictionaryArgs filter_dictionary_args;
    filter_dictionary_args.set_dictionary_name(options.main_dictionary_name);
    filter_dictionary_args.set_dictionary_target_name(options.main_dictionary_name);
    bool fraction;
    double value;
    if (parseNumberOrPercent(options.dictionary_min_df, &value, &fraction))  {
      if (fraction) filter_dictionary_args.set_min_df_rate(value);
      else filter_dictionary_args.set_min_df(value);
    } else {
      if (!options.dictionary_min_df.empty())
        std::cerr << "Error in parameter 'dictionary_min_df', the option will be ignored (" << options.dictionary_min_df << ")\n";
    }
    if (parseNumberOrPercent(options.dictionary_max_df, &value, &fraction))  {
      if (fraction) filter_dictionary_args.set_max_df_rate(value);
      else filter_dictionary_args.set_max_df(value);
    } else {
      if (!options.dictionary_max_df.empty())
        std::cerr << "Error in parameter 'dictionary_max_df', the option will be ignored (" << options.dictionary_max_df << ")\n";
    }

    master_component->FilterDictionary(filter_dictionary_args);
  }

  // Step 4. Configure regularizers.
  std::vector<std::shared_ptr<artm::Regularizer>> regularizers;
  std::map<std::string, std::string> dictionary_map;
  if (!options.use_dictionary.empty())
    dictionary_map.insert(std::make_pair(options.use_dictionary, options.main_dictionary_name));
  for (auto& regularizer : options.regularizer) {
    ::artm::RegularizerConfig regularizer_config;
    configureRegularizer(regularizer, options.topics, &dictionary_map, &regularize_model_args, &process_batches_args, &regularizer_config);
    regularizers.push_back(std::make_shared<artm::Regularizer>(*master_component, regularizer_config));
  }

  // Step 4.1. Configure scores.
  ScoreHelper score_helper(master_component.get(), &dictionary_map);
  ScoreHelper final_score_helper(master_component.get(), &dictionary_map);
  for (auto& score : options.score) score_helper.addScore(score, options.topics);
  for (auto& score : options.final_score) final_score_helper.addScore(score, options.topics);

  // Step 4.2. Loading remaining dictionaries.
  for (auto iter : dictionary_map) {
    if (iter.second == options.main_dictionary_name)
      continue;  // already loaded at step 3.1
    ProgressScope scope(std::string("Importing dictionary ") + iter.first + " with ID=" + iter.second);
    ImportDictionaryArgs import_dictionary_args;
    import_dictionary_args.set_file_name(iter.first);
    import_dictionary_args.set_dictionary_name(iter.second);
    master_component->ImportDictionary(import_dictionary_args);
  }

  // Step 5. Create and initialize model.
  if (options.load_model.empty()) {
    ProgressScope scope("Initializing random model from dictionary");
    InitializeModelArgs initialize_model_args;
    initialize_model_args.set_model_name(pwt_model_name);
    for (auto& topic_name : topic_names)
      initialize_model_args.add_topic_name(topic_name);

    initialize_model_args.set_dictionary_name(options.main_dictionary_name);
    master_component->InitializeModel(initialize_model_args);
  }
  else {
    ProgressScope scope(std::string("Loading model from ") + options.load_model);
    ImportModelArgs import_model_args;
    import_model_args.set_model_name(pwt_model_name);
    import_model_args.set_file_name(options.load_model);
    master_component->ImportModel(import_model_args);
  }

  ::artm::GetTopicModelArgs get_model_args;
  get_model_args.set_request_type(::artm::GetTopicModelArgs_RequestType_Tokens);
  get_model_args.set_model_name(pwt_model_name);
  std::shared_ptr< ::artm::TopicModel> topic_model = master_component->GetTopicModel(get_model_args);
  std::cerr << "Number of tokens in the model: " << topic_model->token_size() << std::endl;

  std::vector<std::string> batch_file_names = findFilesInDirectory(batch_vectorizer.batch_folder(), ".batch");
  int update_count = 0;
  std::cerr << "================= Processing started.\n";
  for (int iter = 0; iter < options.passes; ++iter) {
    CuckooWatch timer("================= Iteration " + boost::lexical_cast<std::string>(iter + 1) + " took ");

    ArtmExecutor executor(options, batch_file_names, master_component.get(), &process_batches_args, &regularize_model_args);
    if (online && options.async) executor.ExecuteAsyncOnlineAlgorithm(&update_count);
    else if (online) executor.ExecuteOnlineAlgorithm(&update_count);
    else executor.ExecuteOfflineAlgorithm();

    score_helper.showScores(pwt_model_name);
  }  // iter

  final_score_helper.showScores(pwt_model_name);

  if (!options.save_dictionary.empty()) {
    ProgressScope scope(std::string("Saving dictionary to ") + options.save_dictionary);
    ExportDictionaryArgs export_dictionary_args;
    export_dictionary_args.set_dictionary_name(options.main_dictionary_name);
    export_dictionary_args.set_file_name(options.save_dictionary);
    master_component->ExportDictionary(export_dictionary_args);
  }

  if (!options.save_model.empty()) {
    ProgressScope scope(std::string("Saving model to ") + options.save_model);
    ExportModelArgs export_model_args;
    export_model_args.set_model_name(pwt_model_name);
    export_model_args.set_file_name(options.save_model);
    master_component->ExportModel(export_model_args);
  }

  if (!options.write_model_readable.empty()) {
    ProgressScope scope(std::string("Saving model in readable format to ") + options.write_model_readable);
    CsvEscape escape(options.csv_separator.size() == 1 ? options.csv_separator[0] : '\0');
    ::artm::Matrix matrix;
    std::shared_ptr< ::artm::TopicModel> model = master_component->GetTopicModel(pwt_model_name, &matrix);
    if (matrix.no_columns() != model->topics_count())
      throw "internal error (matrix.no_columns() != theta->topics_count())";

    std::ofstream output(options.write_model_readable);
    const std::string sep = options.csv_separator;

    // header
    output << "token" << sep << "class_id";
    for (int j = 0; j < model->topics_count(); ++j) {
      if (model->topic_name_size() > 0)
        output << sep << escape.apply(model->topic_name(j));
      else
        output << sep << "topic" << j;
    }
    output << std::endl;

    // bulk
    for (int i = 0; i < model->token_size(); ++i) {
      output << escape.apply(model->token(i)) << sep;
      output << (model->class_id_size() == 0 ? "" : escape.apply(model->class_id(i)));
      for (int j = 0; j < model->topics_count(); ++j) {
        output << sep << matrix(i, j);
      }
      output << std::endl;
    }
  }

  ::artm::Matrix theta_matrix;
  std::shared_ptr< ::artm::ThetaMatrix> theta_metadata;
  if (!options.write_class_predictions.empty() || !options.write_predictions.empty()) {
    {
      ProgressScope scope(std::string("Generating model predictions"));
      if (!master_component->mutable_config()->cache_theta()) {
        master_component->mutable_config()->set_cache_theta(true);
        master_component->Reconfigure(master_component->config());
      }

      process_batches_args.set_pwt_source_name(pwt_model_name);
      process_batches_args.clear_nwt_target_name();

      for (auto& batch_filename : batch_file_names)
        process_batches_args.add_batch_filename(batch_filename);
      master_component->ProcessBatches(process_batches_args);
      process_batches_args.clear_batch_filename();

      theta_metadata = master_component->GetThetaMatrix(pwt_model_name, &theta_matrix);
      if (theta_matrix.no_columns() != theta_metadata->topics_count())
        throw "internal error (matrix.no_columns() != theta->topics_count())";
    }

    score_helper.showScores(pwt_model_name);
  }

  if (!options.write_predictions.empty())
    WritePredictions(options, *theta_metadata, theta_matrix);

  if (!options.write_class_predictions.empty())
    WriteClassPredictions(options, *theta_metadata, theta_matrix);

  return 0;
}

int main(int argc, char * argv[]) {
  try {
    artm_options options;

    po::options_description all_options("BigARTM - library for advanced topic modeling (http://bigartm.org)");

    po::options_description input_data_options("Input data");
    input_data_options.add_options()
      ("read-vw-corpus,c", po::value(&options.read_vw_corpus), "Raw corpus in Vowpal Wabbit format")
      ("read-uci-docword,d", po::value(&options.read_uci_docword), "docword file in UCI format")
      ("read-uci-vocab,v", po::value(&options.read_uci_vocab), "vocab file in UCI format")
      ("read-cooc", po::value(&options.read_cooc), "read co-occurrences format")
      ("batch-size", po::value(&options.batch_size)->default_value(500), "number of items per batch")
      ("use-batches", po::value(&options.use_batches), "folder with batches to use")
    ;

    po::options_description dictionary_options("Dictionary");
    dictionary_options.add_options()
      ("dictionary-min-df", po::value(&options.dictionary_min_df)->default_value(""), "filter out tokens present in less than N documents / less than P% of documents")
      ("dictionary-max-df", po::value(&options.dictionary_max_df)->default_value(""), "filter out tokens present in less than N documents / less than P% of documents")
      ("use-dictionary", po::value(&options.use_dictionary)->default_value(""), "filename of binary dictionary file to use")
    ;

    po::options_description model_options("Model");
    model_options.add_options()
      ("load-model", po::value(&options.load_model)->default_value(""), "load model from file before processing")
      ("topics,t", po::value(&options.topics)->default_value("16"), "number of topics")
      ("use-modality", po::value< std::string >(&options.use_modality)->default_value(""), "modalities (class_ids) and their weights")
      ("predict-class", po::value< std::string >(&options.predict_class)->default_value(""), "target modality to predict by theta matrix")
    ;

    po::options_description learning_options("Learning");
    learning_options.add_options()
      ("passes,p", po::value(&options.passes)->default_value(10), "number of outer iterations")
      ("inner-iterations-count", po::value(&options.inner_iterations_count)->default_value(10), "number of inner iterations")
      ("update-every", po::value(&options.update_every)->default_value(0), "[online algorithm] requests an update of the model after update_every document")
      ("tau0", po::value(&options.tau0)->default_value(1024), "[online algorithm] weight option from online update formula")
      ("kappa", po::value(&options.kappa)->default_value(0.7f), "[online algorithm] exponent option from online update formula")
      ("reuse-theta", po::bool_switch(&options.b_reuse_theta)->default_value(false), "reuse theta between iterations")
      ("regularizer", po::value< std::vector<std::string> >(&options.regularizer)->multitoken(), "regularizers (SmoothPhi,SparsePhi,SmoothTheta,SparseTheta,Decorrelation)")
      ("threads", po::value(&options.threads)->default_value(0), "number of concurrent processors (default: auto-detect)")
      ("async", po::bool_switch(&options.async)->default_value(false), "invoke asynchronous version of the online algorithm")
    ;

    po::options_description output_options("Output");
    output_options.add_options()
      ("save-model", po::value(&options.save_model)->default_value(""), "save the model to binary file after processing")
      ("save-batches", po::value(&options.save_batches)->default_value(""), "batch folder")
      ("save-dictionary", po::value(&options.save_dictionary)->default_value(""), "filename of dictionary file")
      ("write-model-readable", po::value(&options.write_model_readable)->default_value(""), "output the model in a human-readable format")
      ("write-predictions", po::value(&options.write_predictions)->default_value(""), "write prediction in a human-readable format")
      ("write-class-predictions", po::value(&options.write_class_predictions)->default_value(""), "write class prediction in a human-readable format")
      ("csv-separator", po::value(&options.csv_separator)->default_value(";"), "columns separator for --write-model-readable and --write-predictions. Use \\t or TAB to indicate tab.")
      ("score-level", po::value< int >(&options.score_level)->default_value(2), "score level (0, 1, 2, or 3")
      ("score", po::value< std::vector<std::string> >(&options.score)->multitoken(), "scores (Perplexity, SparsityTheta, SparsityPhi, TopTokens, ThetaSnippet, or TopicKernel)")
      ("final-score", po::value< std::vector<std::string> >(&options.final_score)->multitoken(), "final scores (same as scores)")
    ;

    po::options_description ohter_options("Other options");
    ohter_options.add_options()
      ("help,h", "display this help message")
      ("response-file", po::value<std::string>(&options.response_file)->default_value(""), "response file")
      ("paused", po::bool_switch(&options.b_paused)->default_value(false), "start paused and waits for a keystroke (allows to attach a debugger)")
      ("disk-cache-folder", po::value(&options.disk_cache_folder)->default_value(""), "disk cache folder")
      ("disable-avx-opt", po::bool_switch(&options.b_disable_avx_opt)->default_value(false), "disable AVX optimization (gives similar behavior of the Processor component to BigARTM v0.5.4)")
      ("use-dense-bow", po::bool_switch(&options.b_use_dense_bow)->default_value(false), "use dense representation of bag-of-words data in processors")
    ;

    all_options.add(input_data_options);
    all_options.add(dictionary_options);
    all_options.add(model_options);
    all_options.add(learning_options);
    all_options.add(output_options);
    all_options.add(ohter_options);

    po::variables_map vm;
    store(po::command_line_parser(argc, argv).options(all_options).run(), vm);
    notify(vm);

    if (vm.count("response-file") && !options.response_file.empty()) {
      // Load the file and tokenize it
      std::ifstream ifs(vm["response-file"].as<std::string>().c_str());
      if (!ifs) {
        std::cerr << "Could not open the response file\n";
        return 1;
      }

      // Read the whole file into a string
      std::stringstream ss;
      ss << ifs.rdbuf();

      // Split the file content
      // http://stackoverflow.com/questions/541561/using-boost-tokenizer-escaped-list-separator-with-different-parameters
      std::string separator1("");//dont let quoted arguments escape themselves
      std::string separator2(" \n\r");//split on spaces and new lines
      std::string separator3("\"\'");//let it have quoted arguments
      boost::escaped_list_separator<char> els(separator1, separator2, separator3);

      std::string ResponsefileContents(ss.str());
      boost::tokenizer<boost::escaped_list_separator<char>> tok(ResponsefileContents, els);
      std::vector<std::string> args;
      copy(tok.begin(), tok.end(), back_inserter(args));

      // Parse the file and store the options
      store(po::command_line_parser(args).options(all_options).run(), vm);
      notify(vm);
    }

    // Uncomment next two lines to override commandline settings by code. DON'T COMMIT such change to git.
    // options.read_uci_docword = "D:\\datasets\\docword.kos.txt";
    // options.read_uci_vocab   = "D:\\datasets\\vocab.kos.txt";

    bool show_help = (vm.count("help") > 0);

    // Show help if user neither provided batch folder, nor docword/vocab files
    if (options.read_vw_corpus.empty() && options.read_uci_docword.empty() && options.use_batches.empty())
      show_help = true;

    if (show_help) {
      std::cerr << all_options;

      std::cerr << "\nExamples:\n";
      std::cerr << "\tbigartm -d docword.kos.txt -v vocab.kos.txt\n";
      std::cerr << "\tset GLOG_logtostderr=1 & bigartm -d docword.kos.txt -v vocab.kos.txt\n";
      return 1;
    }

    fixScoreLevel(&options);
    fixOptions(&options);

    return execute(options);
  } catch (std::exception& e) {
    std::cerr << "Exception  : " << e.what() << "\n";
    return 1;
  }
  catch (...) {
    std::cerr << "Unknown error occurred.";
    return 1;
  }

  return 0;
}