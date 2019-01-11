#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>
#endif  // USE_OPENCV
#include <stdint.h>

#include <algorithm>
#include <map>
#include <vector>

#include "caffe/data_transformer.hpp"
#include "caffe/layers/annotated_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/sampler.hpp"
#include "caffe/util/im_transforms.hpp"
const float prob_eps = 0.01;
namespace caffe {

template <typename Dtype>
AnnotatedDataLayer<Dtype>::AnnotatedDataLayer(const LayerParameter& param)
  : BasePrefetchingDataLayer<Dtype>(param),
    reader_(param) {
}

template <typename Dtype>
AnnotatedDataLayer<Dtype>::~AnnotatedDataLayer() {
  this->StopInternalThread();
}

template <typename Dtype>
void AnnotatedDataLayer<Dtype>::DataLayerSetUp(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  const int batch_size = this->layer_param_.data_param().batch_size();
  
  const AnnotatedDataParameter& anno_data_param =
      this->layer_param_.annotated_data_param();
  for (int i = 0; i < anno_data_param.batch_sampler_size(); ++i) {
    batch_samplers_.push_back(anno_data_param.batch_sampler(i));
  }
  label_map_file_ = anno_data_param.label_map_file();
  yolo_data_type_ = anno_data_param.yolo_data_type();
  yolo_data_jitter_ = anno_data_param.yolo_data_jitter();
  train_diffcult_ = anno_data_param.train_diffcult();
  // Make sure dimension is consistent within batch.
  const TransformationParameter& transform_param =
    this->layer_param_.transform_param();
  if (transform_param.resize_param_size()) {
    if (transform_param.resize_param(0).resize_mode() ==
        ResizeParameter_Resize_mode_FIT_SMALL_SIZE) {
      CHECK_EQ(batch_size, 1)
        << "Only support batch size of 1 for FIT_SMALL_SIZE.";
    }
  }
  iters_ = 0;
  policy_num_ = 0;
  // Read a data point, and use it to initialize the top blob.
  AnnotatedDatum& anno_datum = *(reader_.full().peek());

  // Use data_transformer to infer the expected blob shape from anno_datum.
  vector<int> top_shape =
      this->data_transformer_->InferBlobShape(anno_datum.datum(),0);
  this->transformed_data_.Reshape(top_shape);
  // Reshape top[0] and prefetch_data according to the batch_size.
  top_shape[0] = batch_size;
  top[0]->Reshape(top_shape);
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].data_.Reshape(top_shape);
  }
  LOG(INFO) << "output data size: " << top[0]->num() << ","
      << top[0]->channels() << "," << top[0]->height() << ","
      << top[0]->width();
  // label
  if (this->output_labels_) {
    has_anno_type_ = anno_datum.has_type() || anno_data_param.has_anno_type();
    vector<int> label_shape(4, 1);
    if (has_anno_type_) {
      anno_type_ = anno_datum.type();
      if (anno_data_param.has_anno_type()) {
        // If anno_type is provided in AnnotatedDataParameter, replace
        // the type stored in each individual AnnotatedDatum.
        LOG(WARNING) << "type stored in AnnotatedDatum is shadowed.";
        anno_type_ = anno_data_param.anno_type();
      }
      // Infer the label shape from anno_datum.AnnotationGroup().
      int num_bboxes = 0;
      if (anno_type_ == AnnotatedDatum_AnnotationType_BBOX) {
        // Since the number of bboxes can be different for each image,
        // we store the bbox information in a specific format. In specific:
        // All bboxes are stored in one spatial plane (num and channels are 1)
        // And each row contains one and only one box in the following format:
        // [item_id, group_label, instance_id, xmin, ymin, xmax, ymax, diff]
        // Note: Refer to caffe.proto for details about group_label and
        // instance_id.
        for (int g = 0; g < anno_datum.annotation_group_size(); ++g) {
          num_bboxes += anno_datum.annotation_group(g).annotation_size();
        }
        label_shape[0] = 1;

        label_shape[1] = 1;
        // BasePrefetchingDataLayer<Dtype>::LayerSetUp() requires to call
        // cpu_data and gpu_data for consistent prefetch thread. Thus we make
        // sure there is at least one bbox.
        label_shape[2] = std::max(num_bboxes, 1);
        label_shape[3] = 8;
		if (yolo_data_type_ == 1) {
			label_shape[2] = 300;
			label_shape[0] = batch_size;
			label_shape[3] = 5;
		}
      } else {
        LOG(FATAL) << "Unknown annotation type.";
      }
    } else {
      label_shape[0] = batch_size;
    }
    top[1]->Reshape(label_shape);
    for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
      this->prefetch_[i].label_.Reshape(label_shape);
    }
  }
}

// This function is called on prefetch thread
template<typename Dtype>
void AnnotatedDataLayer<Dtype>::load_batch(Batch<Dtype>* batch) {
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;
  CHECK(batch->data_.count());
  CHECK(this->transformed_data_.count());

  // Reshape according to the first anno_datum of each batch
  // on single input batches allows for inputs of varying dimension.
  const int batch_size = this->layer_param_.data_param().batch_size();
  const AnnotatedDataParameter& anno_data_param =
      this->layer_param_.annotated_data_param();
  const TransformationParameter& transform_param =
    this->layer_param_.transform_param();
  AnnotatedDatum& anno_datum = *(reader_.full().peek());
  // Use data_transformer to infer the expected blob shape from anno_datum.
  int num_resize_policies = transform_param.resize_param_size();
  bool size_change = false;
  if (num_resize_policies > 0 && iters_%10 == 0) {
	  std::vector<float> probabilities;
	  float prob_sum = 0;
	  for (int i = 0; i < num_resize_policies; i++) {
		  const float prob = transform_param.resize_param(i).prob();
		  CHECK_GE(prob, 0);
		  CHECK_LE(prob, 1);
		  prob_sum += prob;
		  probabilities.push_back(prob);
	  }
	  CHECK_NEAR(prob_sum, 1.0, prob_eps);
	  policy_num_ = roll_weighted_die(probabilities);
	  size_change = true;
  }
  else {

  }
  vector<int> top_shape =
	  this->data_transformer_->InferBlobShape(anno_datum.datum(), policy_num_);
  // Reshape batch according to the batch_size.
  this->transformed_data_.Reshape(top_shape);
  top_shape[0] = batch_size;
  batch->data_.Reshape(top_shape);
  //LOG(INFO) << this->prefetch_[i].data_.width() << "," << transform_param.resize_param(policy_num_).width() << "," << iters_;
  this->prefetch_[0].data_.Reshape(top_shape);
  Dtype* top_data = batch->data_.mutable_cpu_data();
  Dtype* top_label = NULL;  // suppress warnings about uninitialized variables
  if (this->output_labels_ && !has_anno_type_) {
    top_label = batch->label_.mutable_cpu_data();
  }

  // Store transformed annotation.
  map<int, vector<AnnotationGroup> > all_anno;
  int num_bboxes = 0;

  for (int item_id = 0; item_id < batch_size; ++item_id) {
    timer.Start();
    // get a anno_datum
    AnnotatedDatum& anno_datum = *(reader_.full().pop("Waiting for data"));
    read_time += timer.MicroSeconds();
    timer.Start();
    AnnotatedDatum distort_datum;
    AnnotatedDatum* expand_datum = NULL;
    if (transform_param.has_distort_param()) {
      distort_datum.CopyFrom(anno_datum);
      this->data_transformer_->DistortImage(anno_datum.datum(),
                                            distort_datum.mutable_datum());
      if (transform_param.has_expand_param()) {
        expand_datum = new AnnotatedDatum();
        this->data_transformer_->ExpandImage(distort_datum, expand_datum);
      } else {
        expand_datum = &distort_datum;
      }
    } else {
      if (transform_param.has_expand_param()) {
        expand_datum = new AnnotatedDatum();
        this->data_transformer_->ExpandImage(anno_datum, expand_datum);
      } else {
        expand_datum = &anno_datum;
      }
    }
    AnnotatedDatum* sampled_datum = NULL;
    bool has_sampled = false;
    if (batch_samplers_.size() > 0 || yolo_data_type_== 1) {
      // Generate sampled bboxes from expand_datum.
      vector<NormalizedBBox> sampled_bboxes;
	  if (batch_samplers_.size()>0) {
	  	GenerateBatchSamples(*expand_datum, batch_samplers_, &sampled_bboxes);
	  }
	  else {
	    bool keep = transform_param.resize_param(policy_num_).resize_mode() == ResizeParameter_Resize_mode_FIT_LARGE_SIZE_AND_PAD;
	    GenerateJitterSamples(yolo_data_jitter_, &sampled_bboxes , keep);
	  }
      if (sampled_bboxes.size() > 0) {
        // Randomly pick a sampled bbox and crop the expand_datum.
        int rand_idx = caffe_rng_rand() % sampled_bboxes.size();
        sampled_datum = new AnnotatedDatum();
        this->data_transformer_->CropImage(*expand_datum,
                                           sampled_bboxes[rand_idx],
                                           sampled_datum);
        has_sampled = true;
      } else {
        sampled_datum = expand_datum;
      }
    } else {
      sampled_datum = expand_datum;
    }
    CHECK(sampled_datum != NULL);

    vector<int> shape =
        this->data_transformer_->InferBlobShape(sampled_datum->datum(), policy_num_);

	//LOG(INFO) << shape[2] << "," << shape[3];
    if (transform_param.resize_param_size()) {
      if (transform_param.resize_param(policy_num_).resize_mode() ==
          ResizeParameter_Resize_mode_FIT_SMALL_SIZE) {
        this->transformed_data_.Reshape(shape);
        batch->data_.Reshape(shape);
        top_data = batch->data_.mutable_cpu_data();
      } else {
		  //LOG(INFO) << top_shape;
        //CHECK(std::equal(top_shape.begin() + 1, top_shape.begin() + 4,
        //      shape.begin() + 1));
      }
    } else {
      CHECK(std::equal(top_shape.begin() + 1, top_shape.begin() + 4,
            shape.begin() + 1));
    }
    // Apply data transformations (mirror, scale, crop...)
    int offset = batch->data_.offset(item_id);
    this->transformed_data_.set_cpu_data(top_data + offset);
    vector<AnnotationGroup> transformed_anno_vec;
    if (this->output_labels_) {
      if (has_anno_type_) {
        // Make sure all data have same annotation type.
        CHECK(sampled_datum->has_type()) << "Some datum misses AnnotationType.";
        if (anno_data_param.has_anno_type()) {
          sampled_datum->set_type(anno_type_);
        } else {
          CHECK_EQ(anno_type_, sampled_datum->type()) <<
              "Different AnnotationType.";
        }
        // Transform datum and annotation_group at the same time
        transformed_anno_vec.clear();
		//LOG(INFO) << "test";
        this->data_transformer_->Transform(*sampled_datum,
                                           &(this->transformed_data_),
                                           &transformed_anno_vec, policy_num_);
        if (anno_type_ == AnnotatedDatum_AnnotationType_BBOX) {
          // Count the number of bboxes.
          for (int g = 0; g < transformed_anno_vec.size(); ++g) {
            num_bboxes += transformed_anno_vec[g].annotation_size();
          }
        } else {
          LOG(FATAL) << "Unknown annotation type.";
        }
        all_anno[item_id] = transformed_anno_vec;
      } else {
        this->data_transformer_->Transform(sampled_datum->datum(),
                                           &(this->transformed_data_));
        // Otherwise, store the label from datum.
        CHECK(sampled_datum->datum().has_label()) << "Cannot find any label.";
        top_label[item_id] = sampled_datum->datum().label();
      }
    } else {
      this->data_transformer_->Transform(sampled_datum->datum(),
                                         &(this->transformed_data_));
    }
    // clear memory
    if (has_sampled) {
      delete sampled_datum;
    }
    if (transform_param.has_expand_param()) {
      delete expand_datum;
    }
    trans_time += timer.MicroSeconds();

    reader_.free().push(const_cast<AnnotatedDatum*>(&anno_datum));
  }

  // Store "rich" annotation if needed.
  if (this->output_labels_ && has_anno_type_) {
    vector<int> label_shape(4);
    if (anno_type_ == AnnotatedDatum_AnnotationType_BBOX) {
      label_shape[0] = 1;
      label_shape[1] = 1;
      label_shape[3] = 8;
	  if (yolo_data_type_ == 1) {
		  label_shape[0] = batch_size;
		  label_shape[3] = 5;
	  }
      if (num_bboxes == 0) {
        // Store all -1 in the label.
		if (yolo_data_type_ == 1) {
			label_shape[2] = 300;
			batch->label_.Reshape(label_shape);
			caffe_set<Dtype>(8, 0, batch->label_.mutable_cpu_data());
		}
		else {
			label_shape[2] = 1;
			batch->label_.Reshape(label_shape);
			caffe_set<Dtype>(8, -1, batch->label_.mutable_cpu_data());
		}

      } else {
		if (num_bboxes > 300) {
			LOG(INFO) << num_bboxes;
		}
        // Reshape the label and store the annotation.
		if (yolo_data_type_ == 1) {
			label_shape[2] = 300;
			//LOG(INFO) << "num_bboxes: " << num_bboxes;
			batch->label_.Reshape(label_shape);
			
		}
		else {
			label_shape[2] = num_bboxes;
			batch->label_.Reshape(label_shape);
		}

        top_label = batch->label_.mutable_cpu_data();
        int idx = 0;
        for (int item_id = 0; item_id < batch_size; ++item_id) {
          const vector<AnnotationGroup>& anno_vec = all_anno[item_id];
		  if (yolo_data_type_ == 1) {
			  int label_offset = batch->label_.offset(item_id);
			  idx = label_offset;
			  caffe_set(300 * 5, Dtype(0), &top_label[idx]);
		  }

          for (int g = 0; g < anno_vec.size(); ++g) {
            const AnnotationGroup& anno_group = anno_vec[g];
            for (int a = 0; a < anno_group.annotation_size(); ++a) {
              const Annotation& anno = anno_group.annotation(a);
              const NormalizedBBox& bbox = anno.bbox();
			  if (yolo_data_type_ == 1) {  

				  //LOG(INFO) << "difficult: " << bbox.difficult() << ", train_difficult: " << train_diffcult_;
				  if (!bbox.difficult() || train_diffcult_) {
					  float x = (bbox.xmin() + bbox.xmax()) / 2.0;
					  float y = (bbox.ymin() + bbox.ymax()) / 2.0;
					  float w = bbox.xmax() - bbox.xmin();
					  float h = bbox.ymax() - bbox.ymin();
					  top_label[idx++] = anno_group.group_label() - 1;
					  //LOG(INFO) << "class: " << anno_group.group_label();
					  top_label[idx++] = x;
					  top_label[idx++] = y;
					  top_label[idx++] = w;
					  top_label[idx++] = h;
				  }

			  }
			  else {
				  top_label[idx++] = item_id;
				  top_label[idx++] = anno_group.group_label();
				  top_label[idx++] = anno.instance_id();
				  top_label[idx++] = bbox.xmin();
				  top_label[idx++] = bbox.ymin();
				  top_label[idx++] = bbox.xmax();
				  top_label[idx++] = bbox.ymax();
				  top_label[idx++] = bbox.difficult();
			  }

            }
          }
        }
      }
    } else {
      LOG(FATAL) << "Unknown annotation type.";
    }
  }
  iters_++;
  timer.Stop();
  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(AnnotatedDataLayer);
REGISTER_LAYER_CLASS(AnnotatedData);

}  // namespace caffe
