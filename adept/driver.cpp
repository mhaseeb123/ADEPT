#include "kernel.hpp"
#include "driver.hpp"

#define cudaErrchk(ans)                                                                  \
{                                                                                    \
    gpuAssert((ans), __FILE__, __LINE__);                                            \
}
inline void
gpuAssert(cudaError_t code, const char* file, int line, bool abort = true)
{
    if(code != cudaSuccess)
    {
        fprintf(stderr, "GPUassert: %s %s %d cpu:%d\n", cudaGetErrorString(code), file, line);
        if(abort)
            exit(code);
    }
}

using namespace ADEPT;

unsigned getMaxLength (std::vector<std::string> v)
{
  unsigned maxLength = 0;
  for(auto str : v){
    if(maxLength < str.length()){
      maxLength = str.length();
    }
  }
  return maxLength;
}

struct adept_stream{
	cudaStream_t stream;
};

void driver::initialize(short scores[], ALG_TYPE _algorithm, SEQ_TYPE _sequence, CIGAR _cigar_avail, int _gpu_id, std::vector<std::string> ref_seqs, std::vector<std::string> query_seqs){
	algorithm = _algorithm, sequence = _sequence, cigar_avail = _cigar_avail;
	if(sequence == SEQ_TYPE::DNA){
		match_score = scores[0], mismatch_score = scores[1], gap_start = scores[2], gap_extend = scores[3];
	}

	gpu_id = _gpu_id;
    	cudaErrchk(cudaSetDevice(gpu_id));
    	cudaErrchk(cudaStreamCreate(&(curr_stream.stream)));

    	total_alignments = ref_seqs.size();
    	max_ref_size = getMaxLength(ref_seqs);
    	max_que_size = getMaxLength(query_seqs);
	//host pinned memory for offsets
	cudaErrchk(cudaMallocHost(&offset_ref, sizeof(int) * total_alignments));
	cudaErrchk(cudaMallocHost(&offset_que, sizeof(int) * total_alignments));
	//host pinned memory for sequences
	cudaErrchk(cudaMallocHost(&ref_cstr, sizeof(char) * max_ref_size * total_alignments));
	cudaErrchk(cudaMallocHost(&que_cstr, sizeof(char) * max_que_size * total_alignments));
	// host pinned memory for results
	initialize_alignments(total_alignments);
	//device memory for sequences
	cudaErrchk(cudaMalloc(&ref_cstr_d, sizeof(char) * max_ref_size * total_alignments));
	cudaErrchk(cudaMalloc(&que_cstr_d,  sizeof(char)* max_que_size * total_alignments));
	//device memory for offsets and results
	allocate_gpu_mem(total_alignments);

	//preparing offsets 
	unsigned running_sum = 0;
	for(int i = 0; i < total_alignments; i++){
		running_sum +=ref_seqs[i].size();
		offset_ref[i] = running_sum;
	}
	total_length_ref = offset_ref[total_alignments - 1];

	running_sum = 0;
	for(int i = 0; i < query_seqs.size(); i++){
		running_sum +=query_seqs[i].size();
		offset_que[i] = running_sum; 
	}
	total_length_que = offset_que[total_alignments - 1];

	//moving sequences from vector to cstrings
	unsigned offsetSumA = 0;
	unsigned offsetSumB = 0;

 	for(int i = 0; i < ref_seqs.size(); i++){
		char* seqptrA = ref_cstr + offsetSumA;  
		memcpy(seqptrA, ref_seqs[i].c_str(), ref_seqs[i].size());
		char* seqptrB = que_cstr + offsetSumB;
		memcpy(seqptrB, query_seqs[i].c_str(), query_seqs[i].size());
		offsetSumA += ref_seqs[i].size();
		offsetSumB += query_seqs[i].size();
    }
    	//move data asynchronously to GPU
    	mem_cpy_htd(offset_ref_gpu, offset_query_gpu, offset_ref, offset_que, ref_cstr, ref_cstr_d, que_cstr, que_cstr_d, total_length_ref,  total_length_que); // TODO: add streams
}

void driver::kernel_launch(){
	unsigned minSize = (max_que_size < max_ref_size) ? max_que_size : max_ref_size;
	unsigned totShmem = 3 * (minSize + 1) * sizeof(short);
	unsigned alignmentPad = 4 + (4 - totShmem % 4);
	size_t   ShmemBytes = totShmem + alignmentPad;
	if(ShmemBytes > 48000)
        cudaFuncSetAttribute(kernel::dna_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, ShmemBytes);
	kernel::dna_kernel<<<total_alignments, minSize, ShmemBytes>>>(ref_cstr_d, que_cstr_d, offset_ref_gpu, offset_query_gpu, ref_start_gpu, ref_end_gpu, query_start_gpu, query_end_gpu, scores_gpu, match_score, mismatch_score, gap_start, gap_extend);
}

void driver::mem_cpy_htd(unsigned* offset_ref_gpu, unsigned* offset_query_gpu, unsigned* offsetA_h, unsigned* offsetB_h, char* strA, char* strA_d, char* strB, char* strB_d, unsigned totalLengthA, unsigned totalLengthB){
	cudaErrchk(cudaMemcpyAsync(offset_ref_gpu, offsetA_h, (total_alignments) * sizeof(int), cudaMemcpyHostToDevice));
    	cudaErrchk(cudaMemcpyAsync(offset_query_gpu, offsetB_h, (total_alignments) * sizeof(int), cudaMemcpyHostToDevice));
    	cudaErrchk(cudaMemcpyAsync(strA_d, strA, totalLengthA * sizeof(char), cudaMemcpyHostToDevice));
    	cudaErrchk(cudaMemcpyAsync(strB_d, strB, totalLengthB * sizeof(char), cudaMemcpyHostToDevice));
}

void driver::mem_copies_dth(short* ref_start_gpu, short* alAbeg, short* query_start_gpu,short* alBbeg, short* scores_gpu ,short* top_scores_cpu){
    	cudaErrchk(cudaMemcpyAsync(alAbeg, ref_start_gpu, total_alignments * sizeof(short), cudaMemcpyDeviceToHost));
	cudaErrchk(cudaMemcpyAsync(alBbeg, query_start_gpu, total_alignments * sizeof(short), cudaMemcpyDeviceToHost));
    	cudaErrchk(cudaMemcpyAsync(top_scores_cpu, scores_gpu, total_alignments * sizeof(short), cudaMemcpyDeviceToHost));
}

void driver::mem_copies_dth_mid(short* ref_end_gpu, short* alAend, short* query_end_gpu, short* alBend){
    	cudaErrchk(cudaMemcpyAsync(alAend, ref_end_gpu, total_alignments * sizeof(short), cudaMemcpyDeviceToHost));
    	cudaErrchk(cudaMemcpyAsync(alBend, query_end_gpu, total_alignments * sizeof(short), cudaMemcpyDeviceToHost));
}

void driver::mem_cpy_dth(){
	mem_copies_dth_mid(ref_end_gpu, ref_end , query_end_gpu, query_end);
	mem_copies_dth(ref_start_gpu, ref_begin, query_start_gpu, query_begin, scores_gpu , top_scores);
}

void driver::initialize_alignments(int max_alignments){
    	cudaErrchk(cudaMallocHost(&(ref_begin), sizeof(short)*max_alignments));
    	cudaErrchk(cudaMallocHost(&(ref_end), sizeof(short)*max_alignments));
    	cudaErrchk(cudaMallocHost(&(query_begin), sizeof(short)*max_alignments));
    	cudaErrchk(cudaMallocHost(&(query_end), sizeof(short)*max_alignments));
    	cudaErrchk(cudaMallocHost(&(top_scores), sizeof(short)*max_alignments));
}

void driver::dealloc_gpu_mem(){
	cudaErrchk(cudaFree(offset_ref_gpu));
    	cudaErrchk(cudaFree(offset_query_gpu));
    	cudaErrchk(cudaFree(ref_start_gpu));
    	cudaErrchk(cudaFree(ref_end_gpu));
    	cudaErrchk(cudaFree(query_start_gpu));
    	cudaErrchk(cudaFree(query_end_gpu));
	cudaErrchk(cudaFree(ref_cstr_d));
    	cudaErrchk(cudaFree(que_cstr_d));
}

void driver::cleanup(){
    	cudaErrchk(cudaFreeHost(offset_ref));
    	cudaErrchk(cudaFreeHost(offset_que));
    	cudaErrchk(cudaFreeHost(ref_cstr));
    	cudaErrchk(cudaFreeHost(que_cstr));
    	dealloc_gpu_mem();
}

void driver::free_results(){
    cudaErrchk(cudaFreeHost(ref_begin));
    cudaErrchk(cudaFreeHost(ref_end));
    cudaErrchk(cudaFreeHost(query_begin));
    cudaErrchk(cudaFreeHost(query_end));
    cudaErrchk(cudaFreeHost(top_scores));
}

void driver::allocate_gpu_mem(){
    cudaErrchk(cudaMalloc(&offset_query_gpu, (total_alignments) * sizeof(int)));
    cudaErrchk(cudaMalloc(&offset_ref_gpu, (total_alignments) * sizeof(int)));
    cudaErrchk(cudaMalloc(&ref_start_gpu, (total_alignments) * sizeof(short)));
    cudaErrchk(cudaMalloc(&ref_end_gpu, (total_alignments) * sizeof(short)));
    cudaErrchk(cudaMalloc(&query_end_gpu, (total_alignments) * sizeof(short)));
    cudaErrchk(cudaMalloc(&query_start_gpu, (total_alignments) * sizeof(short)));
    cudaErrchk(cudaMalloc(&scores_gpu, (total_alignments) * sizeof(short)));
}
