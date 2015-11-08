#ifndef TEXTNET_LAYER_LISTWISE_MEASURE_LAYER_INL_HPP_
#define TEXTNET_LAYER_LISTWISE_MEASURE_LAYER_INL_HPP_

#include <iostream>
#include <fstream>
#include <sstream>
#include <set>
#include <utility>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>

#include <mshadow/tensor.h>
#include "../layer.h"
#include "../op.h"

using namespace std;

namespace textnet {
namespace layer {

bool list_cmp(const pair<float, float> &x1, const pair<float, float> &x2) {
  return x1.first > x2.first; // sort decrease
}

bool list_cmp_label(const pair<float, float> &x1, const pair<float, float> &x2) {
  return x1.second > x2.second; // sort decrease
}

template<typename xpu>
class ListwiseMeasureLayer : public Layer<xpu>{
 public:
  ListwiseMeasureLayer(LayerType type) { this->layer_type = type; }
  virtual ~ListwiseMeasureLayer(void) {}
  
  virtual int BottomNodeNum() { return 2; }
  virtual int TopNodeNum() { return 1; }
  virtual int ParamNodeNum() { return 0; }
  
  virtual void Require() {
    // default value, just set the value you want
    this->defaults["k"] = SettingV(1.0f);
    this->defaults["col"] = SettingV(0);
	this->defaults["batch_size"] = SettingV(1);
    // require value, set to SettingV(),
    // it will force custom to set in config
    this->defaults["method"] = SettingV();
    
    Layer<xpu>::Require();
  }
  
  virtual void SetupLayer(std::map<std::string, SettingV> &setting,
                          const std::vector<Node<xpu>*> &bottom,
                          const std::vector<Node<xpu>*> &top,
                          mshadow::Random<xpu> *prnd) {
    Layer<xpu>::SetupLayer(setting, bottom, top, prnd);
                            
    utils::Check(bottom.size() == BottomNodeNum(),
                  "ListwiseMeasureLayer:bottom size problem."); 
    utils::Check(top.size() == TopNodeNum(),
                  "ListwiseMeasureLayer:top size problem.");
    k = setting["k"].iVal();
    method = setting["method"].sVal();
    col = setting["col"].iVal();
	batch_size = setting["batch_size"].iVal();
    
    utils::Check(method == "MRR" || method == "P@k" || method == "nDCG@k" || method == "MAP" || method == "P@R", 
                  "Parameter [method] must be MRR or P@k or nDCG@k or MAP or P@R.");
  }
  
  virtual void Reshape(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top,
                       bool show_info = false) {
    utils::Check(bottom.size() == BottomNodeNum(),
                  "ListwiseMeasureLayer:bottom size problem."); 
    utils::Check(top.size() == TopNodeNum(),
                  "ListwiseMeasureLayer:top size problem.");
    nbatch = bottom[0]->data.size(0);    
    top[0]->Resize(1, 1, 1, 1, true);

	utils::Check(nbatch % batch_size == 0,
					"ListwiseMeasureLayer:nbatch %% batch_size != 0.");
	list_size = nbatch / batch_size;

    if (show_info) {
		bottom[0]->PrintShape("bottom0");
        top[0]->PrintShape("top0");
    }
  }

  virtual void CheckReshape(const std::vector<Node<xpu>*> &bottom,
                            const std::vector<Node<xpu>*> &top) {
    // Check for reshape
    nbatch = bottom[0]->data.size(0);
	list_size = nbatch / batch_size;
  }

  inline float rank_log(float x) {
    if (x == 1) return 1.0;
	else return log2(x);
  }

  virtual void Forward(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top) {
    using namespace mshadow::expr;
    mshadow::Tensor<xpu, 2> bottom0_data = bottom[0]->data_d2();
    mshadow::Tensor<xpu, 1> bottom1_data = bottom[1]->data_d1();
    mshadow::Tensor<xpu, 1> top_data = top[0]->data_d1();
    
	top_data = 0.0;

	for (int s = 0; s < batch_size; ++s) {
      vector< pair<float, float> > score_list;
      float score = 0.0;
      float check = 0.0;

      for (int i = 0; i < list_size; ++i) {
	    int idx = s * list_size + i;
        if (bottom1_data[idx] == -1)
          break;
        score_list.push_back( make_pair(bottom0_data[idx][col], bottom1_data[idx]) );
      }

      int score_list_len = score_list.size();

	  if (method == "nDCG@k") {
		sort(score_list.begin(), score_list.end(), list_cmp_label);
        idcg = 0.0;
		for (int i = 0; i < min(k, score_list_len); ++i) {
          idcg += score_list[i].second / rank_log(i+1);
		}
	  }

	  // shuffle before sort!
	  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
	  std::shuffle(score_list.begin(), score_list.end(), std::default_random_engine(seed)); 

      sort(score_list.begin(), score_list.end(), list_cmp);

      if (method == "MRR") {
        for (int i = 0; i < score_list_len; ++i) {
		  if (score_list[i].second > 0) 
			  score_list[i].second = 1;
		  utils::Check(score_list[i].second == 0 || score_list[i].second == 1, 
				  "Not a valid list for MRR, only 0 and 1.");
          if (score_list[i].second == 1) {
            score = 1.0 / (i+1);
			break;
		  }
        }
      } else if (method == "P@k") {
        for (int i = 0; i < min(k, score_list_len); ++i) {
		  if (score_list[i].second > 0) 
			  score_list[i].second = 1;
		  utils::Check(score_list[i].second == 0 || score_list[i].second == 1, 
				  "Not a valid list for P@k, only 0 and 1.");
          if (score_list[i].second == 1) {
            score += 1.0;
		  }
        }
		score /= k;
      } else if (method == "P@R") {
		int r = 0;
        for (int i = 0; i < score_list_len; ++i) {
		  if (score_list[i].second > 0) 
			  score_list[i].second = 1;
		  if (score_list[i].second == 1) {
			r += 1;
		  }
		}
		for (int i = 0; i < min(r, score_list_len); ++i) {
		  utils::Check(score_list[i].second == 0 || score_list[i].second == 1, 
				  "Not a valid list for P@R, only 0 and 1.");
          if (score_list[i].second == 1) {
            score += 1.0;
		  }
        }
		if (r == 0) {
			utils::Check(score==0.0, "P@R Error!");
		} else {
			score /= r;
		}
      } else if (method == "nDCG@k") {
        for (int i = 0; i < min(k, score_list_len); ++i) {
          score += score_list[i].second / rank_log(i+1);
        }
		if (idcg == 0) {
			utils::Check(score==0.0, "P@R Error!");
		} else {
			score /= idcg;
		}
      } else if (method == "MAP") {
		int p_count = 0;
		for (int i = 0; i < score_list_len; ++i) {
          score_list[i].first = i;
		}
		sort(score_list.begin(), score_list.end(), list_cmp_label);
		for (int i = 0; i < score_list_len; ++i) {
		  if (score_list[i].second == 0)
			  break;
		  p_count += 1;
          score += i / score_list[i].first;
		}
		score /= p_count;
	  }
      top_data[0] += score;
	}

	top_data[0] /= batch_size;
  }
  
  virtual void Backprop(const std::vector<Node<xpu>*> &bottom,
                        const std::vector<Node<xpu>*> &top) {
    using namespace mshadow::expr;

  }
  
 protected:
  int nbatch;
  int k;
  string method;
  int col;
  int batch_size;
  int list_size;
  float idcg;

};
}  // namespace layer
}  // namespace textnet
#endif  // LAYER_LISTWISE_MEASURE_LAYER_INL_HPP_
