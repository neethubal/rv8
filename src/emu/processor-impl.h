//
//  processor-impl.h
//

#ifndef rv_processor_impl_h
#define rv_processor_impl_h

#include "meta.h"
#include "../../../sniper/sift/sift_format.h"
#include "../../../sniper/sift/sift_writer.h"

namespace riscv {

	/* Interpreter Exit Causes */

	enum exit_cause {
		exit_cause_continue,
		exit_cause_cli,
		exit_cause_poweroff,
	};

	/* Register formatter */

	template <typename T>
	std::string format_reg(std::string name, T &&reg, bool decimal = false)
	{
		if (decimal) {
			return
				sizeof(T) == 4 ? format_string("%-9s:%10d", name.c_str(), reg) :
				sizeof(T) == 8 ? format_string("%-9s:%18lld", name.c_str(), reg) :
				format_string("%-9s:<INVALID>", name.c_str());
		} else {
			return
				sizeof(T) == 4 ? format_string("%-9s:0x%08x", name.c_str(), reg) :
				sizeof(T) == 8 ? format_string("%-9s:0x%016llx", name.c_str(), reg) :
				format_string("%-9s:<INVALID>", name.c_str());
		}
	}

	/* Processor base template */

	typedef google::dense_hash_map<addr_t,size_t> hist_pc_map_t;
	typedef google::dense_hash_map<size_t,size_t> hist_reg_map_t;
	typedef google::dense_hash_map<short,size_t> hist_inst_map_t;
	typedef std::pair<addr_t,size_t> hist_pc_pair_t;
	typedef std::pair<size_t,size_t> hist_reg_pair_t;
	typedef std::pair<size_t,size_t> hist_inst_pair_t;

	template<typename T, typename P, typename M>
	struct processor_impl : P
	{
		typedef T decode_type;
		typedef P processor_type;
		typedef M mmu_type;

		mmu_type mmu;
		hist_pc_map_t hist_pc;
		hist_reg_map_t hist_reg;
		hist_inst_map_t hist_inst;
		std::function<const char*(addr_t)> symlookup;

		processor_impl() : P()
		{
			hist_pc.set_empty_key(0);
			hist_pc.set_deleted_key(-1);
			hist_reg.set_empty_key(-1);
			hist_inst.set_empty_key(-1);
		}

		std::string format_inst(inst_t inst)
		{
			std::string buf;
			switch (inst_length(inst)) {
				case 2:  sprintf(buf, "%04llx    ", inst); break;
				case 4:  sprintf(buf, "%08llx", inst); break;
				case 6:  sprintf(buf, "%012llx", inst); break;
				case 8:  sprintf(buf, "%016llx", inst); break;
				default: sprintf(buf, "(invalid)"); break;
			}
			return buf;
		}

		size_t regnum(T &dec, rv_operand_name operand_name)
		{
			switch (operand_name) {
				case rv_operand_name_rd: return dec.rd;
				case rv_operand_name_rs1: return dec.rs1;
				case rv_operand_name_rs2: return dec.rs2;
				case rv_operand_name_frd: return dec.rd;
				case rv_operand_name_frs1: return dec.rs1;
				case rv_operand_name_frs2: return dec.rs2;
				case rv_operand_name_frs3: return dec.rs3;
				default: return 0;
			}
		}

		void histogram_set_pc(addr_t key, size_t val)
		{
			hist_pc[key] = val;
		}

		size_t histogram_add_pc(addr_t key)
		{
			auto hi = hist_pc.find(key);
			if (hi == hist_pc.end()) {
				hist_pc.insert(hist_pc_pair_t(key, 1));
				return 1;
			} else {
				if (hi->second < P::hostspot_trace_limit) {
					hi->second++;
				}
				return hi->second;
			}
		}

		void histogram_add_reg(size_t key)
		{
			auto hi = hist_reg.find(key);
			if (hi == hist_reg.end()) hist_reg.insert(hist_reg_pair_t(key, 1));
			else hi->second++;
		}

		void histogram_add_regs(decode_type &dec)
		{
			const rv_operand_data *operand_data = rv_inst_operand_data[dec.op];
			while (operand_data->type != rv_type_none) {
				switch (operand_data->type) {
					case rv_type_ireg:
						histogram_add_reg(regnum(dec, operand_data->operand_name));
						break;
					case rv_type_freg:
						histogram_add_reg(32 + regnum(dec, operand_data->operand_name));
						break;
					default: break;
				}
				operand_data++;
			}
		}

		void histogram_add_inst(decode_type &dec)
		{
			size_t op = dec.op;
			auto hi = hist_inst.find(op);
			if (hi == hist_inst.end()) hist_inst.insert(hist_inst_pair_t(op, 1));
			else hi->second++;
		}

		void seed_registers(host_cpu &cpu, uint64_t initial_seed, size_t n)
		{
			sha512_ctx_t sha512;
			u8 seed[SHA512_OUTPUT_BYTES];   /* 512-bits random seed */
			u8 random[SHA512_OUTPUT_BYTES]; /* 512-bits hash output */

			/* if 64-bit initial seed is specified, repeat seed 8 times in the seed buffer
			   if no initial seed is specified then fill the seed buffer with 512-bits of random */
			for (size_t i = 0; i < SHA512_OUTPUT_BYTES; i += 8) {
				*(u64*)(seed + i) = initial_seed ? initial_seed
					: (((u64)cpu.get_random_seed()) << 32) | (u64)cpu.get_random_seed() ;
			}

			/* Log initial seed state */
			if (P::log & proc_log_memory) {
				std::string seed_str;
				for (size_t i = 0; i < SHA512_OUTPUT_BYTES; i += 8) {
					seed_str.append(format_string("%016llx", *(u64*)(seed + i)));
				}
				debug("seed: %s", seed_str.c_str());
			}

			/* Randomize the integer registers */
			size_t rand_bytes = 0;
			std::uniform_int_distribution<typename P::ux> distribution(0, std::numeric_limits<typename P::ux>::max());
			for (size_t i = rv_ireg_x1; i < P::ireg_count; i++) {
				/* on buffer exhaustion sha-512 hash the seed and xor the hash back into the seed */
				if ((rand_bytes & (SHA512_OUTPUT_BYTES - 1)) == 0) {
					sha512_init(&sha512);
					sha512_update(&sha512, seed, SHA512_OUTPUT_BYTES);
					sha512_final(&sha512, random);
					for (size_t i = 0; i < SHA512_OUTPUT_BYTES; i += 8) {
						*(u64*)(seed + i) ^= *(u64*)(random + i);
					}
				}
				P::ireg[i].r.xu.val = *(u64*)(random + (rand_bytes & (SHA512_OUTPUT_BYTES - 1)));
				rand_bytes += 8;
			}
		}

		std::string format_operands(T &dec)
		{
			size_t reg;
			std::string op;
			std::map<std::string,std::string> ops;
			const rv_operand_data *operand_data = rv_inst_operand_data[dec.op];
			while (operand_data->type != rv_type_none) {
				op.clear();
				switch (operand_data->type) {
					case rv_type_ireg:
						reg = regnum(dec, operand_data->operand_name);
						sprintf(op, "0x");
						sprintf(op,
							rv_type_primitives[operand_data->primitive].hex_format,
							P::ireg[reg].r.xu.val);
						ops[rv_ireg_name_sym[reg]] = op;
						break;
					case rv_type_freg:
						reg = regnum(dec, operand_data->operand_name);
						sprintf(op, "%s=", rv_freg_name_sym[reg]);
						// show hex value for +/-{inf|subnorm|nan}
						if (operand_data->primitive == rv_primitive_f64 ?
							(f64_classify(P::freg[reg].r.d.val) & 0b1110100101) :
							(f32_classify(P::freg[reg].r.s.val) & 0b1110100101))
						{
							sprintf(op,
								operand_data->primitive == rv_primitive_f64 ?
								"%.17g[%016llx]" : "%.9g[%08llx]",
								operand_data->primitive == rv_primitive_f64 ?
								P::freg[reg].r.d.val : P::freg[reg].r.s.val,
								operand_data->primitive == rv_primitive_f64 ?
								P::freg[reg].r.lu.val : P::freg[reg].r.wu.val);
						} else {
							sprintf(op,
								operand_data->primitive == rv_primitive_f64 ?
								"%.17g" : "%.9g",
								operand_data->primitive == rv_primitive_f64 ?
								P::freg[reg].r.d.val : P::freg[reg].r.s.val);
						}
						ops[rv_freg_name_sym[reg]] = op;
						break;
					default: break;
				}
				operand_data++;
			}

			std::string operands;
			for (auto i = ops.begin(); i != ops.end(); i++) {
				if (i != ops.begin()) {
					operands.append(", ");
				}
				operands.append(i->first);
				operands.append("=");
				operands.append(i->second);
			}
			return operands;
		}

		void print_log(decode_type &dec, inst_t inst)
		{
			static const char *fmt_32 = "%019llu core-%-4zu:%08llx (%s) %-30s %s\n";
			static const char *fmt_64 = "%019llu core-%-4zu:%016llx (%s) %-30s %s\n";
			static const char *fmt_128 = "%019llu core-%-4zu:%032llx (%s) %-30s %s\n";
			if (P::log & proc_log_hist_reg) histogram_add_regs(dec);
			if (P::log & proc_log_hist_inst) histogram_add_inst(dec);
			if (P::log & proc_log_inst) {
				std::fexcept_t flags;
				fegetexceptflag(&flags, FE_ALL_EXCEPT);
				if (!(P::log & proc_log_no_pseudo)) decode_pseudo_inst(dec);
				if (symlookup) printf("%32s ", symlookup(P::pc));
				std::string args = disasm_inst_simple(dec);
				std::string op_args = (P::log & proc_log_operands) ? format_operands(dec) : std::string();
				printf(P::xlen == 32 ? fmt_32 : P::xlen == 64 ? fmt_64 : fmt_128,
					P::instret, P::hart_id, addr_t(P::pc), format_inst(inst).c_str(), args.c_str(), op_args.c_str());
				fesetexceptflag(&flags, FE_ALL_EXCEPT);
			}
			if (P::log & proc_log_int_reg) print_int_registers();
		}

		bool isBranch(decode_type &dec)
		{
			bool res = false;
			switch (dec.op) {
				case rv_op_beq:		/* Branch Equal */
				case rv_op_bne:		/* Branch Not Equal */
				case rv_op_blt:		/* Branch Less Than */
				case rv_op_bge:		/* Branch Greater than Equal */
				case rv_op_bltu:	/* Branch Less Than Unsigned */
				case rv_op_bgeu:	/* Branch Greater than Equal Unsigned */
				case rv_op_beqz:	/* Branch if = zero */
				case rv_op_bnez:	/* Branch if ≠ zero */
				case rv_op_blez:	/* Branch if ≤ zero */
				case rv_op_bgez:	/* Branch if ≥ zero */
				case rv_op_bltz:	/* Branch if < zero */
				case rv_op_bgtz:	/* Branch if > zero */
				case rv_op_ble:
				case rv_op_bleu:
				case rv_op_bgt:
				case rv_op_bgtu:
					res = true;
					break;
				default:
					res = false;
					break;
			}
			return res;
		}

		bool isMemoryOp(decode_type &dec)
		{
			bool res = false;
			switch (dec.op) {
					//P::ireg[dec.rs1] + dec.imm
				case rv_op_lb: 			/* Load Byte */
				case rv_op_lh: 			/* Load Half */
				case rv_op_lw: 			/* Load Word */
				case rv_op_lbu: 		/* Load Byte Unsigned */
				case rv_op_lhu: 		/* Load Half Unsigned */
				case rv_op_lwu: 		/* Load Word Unsigned */
				case rv_op_ld: 			/* Load Double */
				case rv_op_flw: 		/* FP Load (SP) */
				case rv_op_fld: 		/* FP Load (DP) */
				case rv_op_sb: 			/* Store Byte */
				case rv_op_sh: 			/* Store Half */
				case rv_op_sw: 			/* Store Word */
				case rv_op_sd: 			/* Store Double */
				case rv_op_fsw: 		/* FP Store (SP) */
				case rv_op_fsd: 		/* FP Store (DP) */
					//P::ireg[dec.rs1]
				case rv_op_lr_w: 	 	/* Load Reserved Word */
				case rv_op_lr_d: 		/* Load Reserved Double Word */
				case rv_op_sc_w: 		/* Store Conditional Word */
				case rv_op_sc_d: 		/* Store Conditional Double Word */
					res = true; 
					break;
				default:
					res = false;
					break;
			}
			return res;
		}

		uint64_t getAddress(decode_type &dec)
		{
			uint64_t addr ;
			printf("dec.op = %d\n",dec.op);
			switch (dec.op) {
				case rv_op_lb: 			/* Load Byte */
				case rv_op_lh: 			/* Load Half */
				case rv_op_lw: 			/* Load Word */
				case rv_op_lbu: 		/* Load Byte Unsigned */
				case rv_op_lhu: 		/* Load Half Unsigned */
				case rv_op_lwu: 		/* Load Word Unsigned */
				case rv_op_ld: 			/* Load Double */
				case rv_op_flw: 		/* FP Load (SP) */
				case rv_op_fld: 		/* FP Load (DP) */
				case rv_op_sb: 			/* Store Byte */
				case rv_op_sh: 			/* Store Half */
				case rv_op_sw: 			/* Store Word */
				case rv_op_sd: 			/* Store Double */
				case rv_op_fsw: 		/* FP Store (SP) */
				case rv_op_fsd: 		/* FP Store (DP) */
					addr = P::ireg[dec.rs1] + dec.imm;
					break;
				case rv_op_lr_w: 	 	/* Load Reserved Word */
				case rv_op_lr_d: 		/* Load Reserved Double Word */
				case rv_op_sc_w: 		/* Store Conditional Word */
				case rv_op_sc_d: 		/* Store Conditional Double Word */
					addr = P::ireg[dec.rs1]; 
					break;
				default:
					printf("No such instruction!");
					break;
			}
			return addr;
		}

		void print_log_PSift(Sift::Writer *output, decode_type &dec, inst_t inst, int new_offset)
		{
			printf ("[src\\emu\\processor-impl.h]\tinside print_log_PSift\n");
			static const char *fmt_32 = "%019llu core-%-4zu:%08llx (%s) %-30s %s\n";
			static const char *fmt_64 = "%019llu core-%-4zu:%016llx (%s) %-30s %s\n";
			static const char *fmt_128 = "%019llu core-%-4zu:%032llx (%s) %-30s %s\n";
			if (P::log & proc_log_hist_reg) histogram_add_regs(dec);
			if (P::log & proc_log_hist_inst) histogram_add_inst(dec);
			if (P::log & proc_log_inst) {
				std::fexcept_t flags;
				fegetexceptflag(&flags, FE_ALL_EXCEPT);
				if (!(P::log & proc_log_no_pseudo)) decode_pseudo_inst(dec);		// src/asm/disasm.h
				if (symlookup) printf("%32s ", symlookup(P::pc));
				std::string args = disasm_inst_simple(dec);
				std::string op_args = (P::log & proc_log_operands) ? format_operands(dec) : std::string();
				printf(P::xlen == 32 ? fmt_32 : P::xlen == 64 ? fmt_64 : fmt_128,
					P::instret, P::hart_id, addr_t(P::pc), format_inst(inst).c_str(), args.c_str(), op_args.c_str());
				
				uint64_t addr           =       addr_t(P::pc);
				uint8_t size            =       inst_length(inst);
				uint8_t num_addresses 	= 		1;
				uint64_t addresses[4];
				bool is_memory          =       isMemoryOp(dec);
				if(is_memory == true) {
					uint64_t addr_mem = getAddress(dec);
					addresses[0] = addr_mem;
					printf("addr_mem => %llx\n",addr_mem);
				}

				bool is_branch          =       isBranch(dec);
				bool taken              =       0;
				if(is_branch == true) {
					int nextpc_offset = intptr_t(dec.imm);
					printf("[src\\emu\\processor-impl.h]\tIsBRANCH -> new offset = %d nextpcoffset = %d \n",new_offset,nextpc_offset);
					if(nextpc_offset==new_offset) {
						taken = 1;
					}
				}

				bool is_predicate       =       0;
				bool executed           =       1;

				printf("[src\\emu\\processor-impl.h]\t%032llx P::xlen=%d INSTR :: %llu instformat=%s args=%s dec.op=%d isBranch=%d taken=%d\n", addr_t(P::pc), size, inst,rv_inst_format[dec.op],args.c_str(),dec.op,is_branch,taken);
				//print_int_registers();
				output->Instruction(addr, size, num_addresses, addresses, is_branch, taken, is_predicate, executed);
				fesetexceptflag(&flags, FE_ALL_EXCEPT);
			}
			if (P::log & proc_log_int_reg) print_int_registers();
		}

		void print_device_registers() {}

		void print_csr_registers()
		{
			printf("%s %s\n", format_reg("instret", P::instret, true).c_str(),
			                  format_reg("time", P::time).c_str());
			printf("%s %s\n", format_reg("pc", P::pc).c_str(),
			                  format_reg("fcsr", P::fcsr).c_str());
		}

		void print_int_registers()
		{
			for (size_t i = rv_ireg_x1; i < P::ireg_count; i++) {
				printf("%s%s", format_reg(rv_ireg_name_sym[i],
					P::ireg[i].r.xu.val).c_str(), /* 3 column layout */
					((i - 1) % 2) == 0 ? "\n" : ((i - 1) % 2) > 0 ? " " : "");
			}
		}

		void print_f32_registers()
		{
			for (size_t i = rv_freg_f0; i < P::freg_count; i++) {
				printf("%-4s: s %16.5f%s", rv_freg_name_sym[i],
					P::freg[i].r.s.val, (i + 1) % 2 == 0 ? "\n" : " ");
			}
		}

		void print_f64_registers()
		{
			for (size_t i = rv_freg_f0; i < P::freg_count; i++) {
				printf("%-4s: d %16.5f%s", rv_freg_name_sym[i],
					P::freg[i].r.d.val, (i + 1) % 2 == 0 ? "\n" : " ");
			}
		}

		template <typename D, typename R, typename V>
		void set_csr(D &dec, int mode, int op, int csr, R &reg, V value,
			const R write_mask = -1, const R read_mask = -1, const size_t shift = 0)
		{
			const int csr_mode = (csr >> 8) & 3, readonly = (csr >> 12) & 1;
			if (dec.rd != rv_ireg_x0) {
				P::ireg[dec.rd] = (mode >= csr_mode) ? (reg >> shift) & read_mask : 0;
			}
			if (readonly) return;
			switch (op) {
				case csr_rw: reg = (reg & ~(write_mask << shift)) | ((value & write_mask) << shift); break;
				case csr_rs: if (value) reg |= ((value & write_mask) << shift); break;
				case csr_rc: if (value) reg &= ~((value & write_mask) << shift); break;
			}
		}

		template <typename D, typename R, typename V>
		void get_csr(D &dec, int mode, int op, int csr, R &reg, V value)
		{
			const int csr_mode = (csr >> 8) & 3;
			if (dec.rd != rv_ireg_x0) {
				P::ireg[dec.rd] = (mode >= csr_mode) ? reg : 0;
			}
		}

		template <typename D, typename R, typename V>
		void set_csr_hi(D &dec, int mode, int op, int csr, R &reg, V value)
		{
			const int csr_mode = (csr >> 8) & 3, readonly = (csr >> 12) & 1;
			if (dec.rd != rv_ireg_x0) {
				P::ireg[dec.rd] = (mode >= csr_mode) ? s32(u32(reg >> 32)) : 0;
			}
			if (readonly) return;
			switch (op) {
				case csr_rw: reg = (u64(value) << 32) | (reg & 0xffffffffU); break;
				case csr_rs: if (value) reg |= (u64(value) << 32); break;
				case csr_rc: if (value) reg &= ~(u64(value) << 32) | 0xffffffffU; break;
			}
		}

		template <typename D, typename R, typename V>
		void get_csr_hi(D &dec, int mode, int op, int csr, R &reg, V value)
		{
			const int csr_mode = (csr >> 8) & 3;
			if (dec.rd != rv_ireg_x0) {
				P::ireg[dec.rd] = (mode >= csr_mode) ? s32(u32(reg >> 32)) : 0;
			}
		}
	};

}

#endif
