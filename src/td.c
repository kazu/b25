#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(WIN32)
	#include <io.h>
	#include <windows.h>
	#include <crtdbg.h>
#else
	#define __STDC_FORMAT_MACROS
	#include <inttypes.h>
	#include <unistd.h>
        #include <errno.h>
	#include <pthread.h>
	#include <sys/poll.h>
	#include <linux/futex.h>
	#include <sys/time.h>
	#include <sys/resource.h>
	#include <signal.h>
#endif


#include "arib_std_b25.h"
#include "b_cas_card.h"

#ifdef _LOADER_DEBUG
# define DEBUG(f,a...) fprintf(stderr, f, ## a)
#else
# define DEBUG(f, a...) do { ; } while(0)
#endif

#ifdef _WAKE_BROADCAST
#define wake_thread(a)  pthread_cond_broadcast(a)
#else
#define wake_thread(a)  pthread_cond_signal(a)
#endif

typedef struct {
	int32_t round;
	int32_t strip;
	int32_t emm;
	int32_t verbose;
	int32_t power_ctrl;
} OPTION;


typedef struct {
  int flag; // 0 - no use .  1 loading. 2 loaded, 3 b25
  void *next;
  int32_t size;
  uint8_t data[8*1024];
} ring_buffer;

typedef struct {
  int sfd;
  ring_buffer *rbuf;
  ARIB_STD_B25 *b25;
} loader_args; 

#define RING_BUF_SIZE  (10)

static void show_usage();
static int parse_arg(OPTION *dst, int argc, char **argv);
static void test_arib_std_b25(const char *src, const char *dst, OPTION *opt);
static void show_bcas_power_on_control_info(B_CAS_CARD *bcas);
static int init_ring_buffer(ring_buffer *rbuf,int size);

int used = 0;
int loader_status = 0;
double r1 = 0;

pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mut2 = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t rcond = PTHREAD_COND_INITIALIZER;
pthread_cond_t wcond = PTHREAD_COND_INITIALIZER;


double gettimeofday_sec()
{
	struct rusage t;
	struct timeval tv;
	getrusage(RUSAGE_SELF, &t);
	tv = t.ru_utime;
	return tv.tv_sec + (double)tv.tv_usec*1e-6;
}

int main(int argc, char **argv)
{
	int n;
	OPTION opt;
	
	#if defined(WIN32)
	_CrtSetReportMode( _CRT_WARN, _CRTDBG_MODE_FILE );
	_CrtSetReportFile( _CRT_WARN, _CRTDBG_FILE_STDOUT );
	_CrtSetReportMode( _CRT_ERROR, _CRTDBG_MODE_FILE );
	_CrtSetReportFile( _CRT_ERROR, _CRTDBG_FILE_STDOUT );
	_CrtSetReportMode( _CRT_ASSERT, _CRTDBG_MODE_FILE );
	_CrtSetReportFile( _CRT_ASSERT, _CRTDBG_FILE_STDOUT );
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF|_CRTDBG_DELAY_FREE_MEM_DF|_CRTDBG_CHECK_ALWAYS_DF|_CRTDBG_LEAK_CHECK_DF);
	#endif

	n = parse_arg(&opt, argc, argv);
	if(n+2 > argc){
		show_usage();
		exit(EXIT_FAILURE);
	}

	test_arib_std_b25(argv[n+0], argv[n+1], &opt);

	#if defined(WIN32)
	_CrtDumpMemoryLeaks();
	#endif

	return EXIT_SUCCESS;
}

static void show_usage()
{
	fprintf(stderr, "b25 - ARIB STD-B25 test program ver. 0.2.3 (2008, 12/30)\n");
	fprintf(stderr, "usage: b25 [options] src.m2t dst.m2t\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "  -r round (integer, default=4)\n");
	fprintf(stderr, "  -s strip\n");
	fprintf(stderr, "     0: keep null(padding) stream (default)\n");
	fprintf(stderr, "     1: strip null stream\n");
	fprintf(stderr, "  -m EMM\n");
	fprintf(stderr, "     0: ignore EMM (default)\n");
	fprintf(stderr, "     1: send EMM to B-CAS card\n");
	fprintf(stderr, "  -p power_on_control_info\n");
	fprintf(stderr, "     0: do nothing additionaly\n");
	fprintf(stderr, "     1: show B-CAS EMM receiving request (default)\n");
	fprintf(stderr, "  -v verbose\n");
	fprintf(stderr, "     0: silent\n");
	fprintf(stderr, "     1: show processing status (default)\n");
	fprintf(stderr, "\n");
}

static int parse_arg(OPTION *dst, int argc, char **argv)
{
	int i;
	
	dst->round = 4;
	dst->strip = 0;
	dst->emm = 0;
	dst->power_ctrl = 1;
	dst->verbose = 1;

	for(i=1;i<argc;i++){
		if(argv[i][0] != '-'){
			break;
		}
		switch(argv[i][1]){
		case 'm':
			if(argv[i][2]){
				dst->emm = atoi(argv[i]+2);
			}else{
				dst->emm = atoi(argv[i+1]);
				i += 1;
			}
			break;
		case 'p':
			if(argv[i][2]){
				dst->power_ctrl = atoi(argv[i]+2);
			}else{
				dst->power_ctrl = atoi(argv[i+1]);
				i += 1;
			}
			break;
		case 'r':
			if(argv[i][2]){
				dst->round = atoi(argv[i]+2);
			}else{
				dst->round = atoi(argv[i+1]);
				i += 1;
			}
			break;
		case 's':
			if(argv[i][2]){
				dst->strip = atoi(argv[i]+2);
			}else{
				dst->strip = atoi(argv[i+1]);
				i += 1;
			}
			break;
		case 'v':
			if(argv[i][2]){
				dst->verbose = atoi(argv[i]+2);
			}else{
				dst->verbose = atoi(argv[i+1]);
				i += 1;
			}
			break;
		default:
			fprintf(stderr, "error - unknown option '-%c'\n", argv[i][1]);
			return argc;
		}
	}

	return i;
}

void *loader_main(void *args)
{
	loader_args *largs = (loader_args *) args;
	int sfd;
	int n;
	ring_buffer *rbuf;
	ARIB_STD_B25 *b25;
	ring_buffer *cbuf;
	struct pollfd pfd[1];
	double r2 = 0;

	sfd = largs->sfd;
	rbuf = largs->rbuf;
	b25 = largs->b25;



        loader_status = 1;
	pfd[0].fd = sfd;
	pfd[0].events = POLLIN;

	//printf("called!\n");

	cbuf = &rbuf[0];
	while(1) {
		if(used ==  RING_BUF_SIZE ){
			//futex(&used,FUTEX_WAIT,2,NULL);
			DEBUG("loader sleep used: %d \n", used);
			wake_thread(&wcond);
			pthread_cond_wait(&rcond, &mut);
			// pthread_cond_broadcast(&wcond);
			DEBUG("loader wake used: %d \n", used);
			continue;	
		}
	  	if(cbuf->flag != 0 ){
			cbuf = cbuf->next;
			//pthread_cond_broadcast(&wcond);
			continue;
		}
		if(poll(pfd, 1, 1)){
		       if(pfd[0].revents & POLLIN){
				cbuf->flag = 1;
			        used++;
				r2 = gettimeofday_sec();
				n = _read(sfd, cbuf->data, sizeof(cbuf->data));
				r1 +=  (gettimeofday_sec() - r2);
				if( n < 1 ){
					used--;
					if( errno == 75 ) {
						 
						DEBUG("warn - failed on read errno %d \n", errno);
						cbuf->flag = 0;
						continue;
					}
					DEBUG( "loader error finish code: %d  ret: %d\n", errno, n);
					cbuf->flag = 0;
					break;
				}
				cbuf->size = n;
				cbuf->flag = 2;
				wake_thread(&wcond);
				DEBUG("wakeup writer from loader used: %d\n",used);
				//futex(&used,FUTEX_WAKE,2,NULL);
		       }
		}
		cbuf = cbuf->next;
	}
	DEBUG("fin loader\n");
	loader_status = -1;
	wake_thread(&wcond);
	return 0;
}

static int init_ring_buffer(ring_buffer *rbuf,int size)
{
	int i;
	for(i =0 ; i < size; i++){
		// printf("%d 0x%x\n", i, (void *)&rbuf[i]);
		rbuf[i].flag = 0;
		if( i == size - 1 ) {
			// printf("last! %d\n",i);
			rbuf[i].next = &rbuf[0];
		}else{
			rbuf[i].next = &rbuf[i+1];
		}

	}
	return 1;
}

static void test_arib_std_b25(const char *src, const char *dst, OPTION *opt)
{
	int code,i,n,m;
	int sfd,dfd;

	int64_t total;
	int64_t offset;

	ARIB_STD_B25 *b25;
	B_CAS_CARD   *bcas;

	ARIB_STD_B25_PROGRAM_INFO pgrm;

	// uint8_t data[8*1024];
	ring_buffer rbuf[RING_BUF_SIZE];
	ring_buffer *cbuf;
	pthread_t loader;
	loader_args thread_args;

	ARIB_STD_B25_BUFFER sbuf;
	ARIB_STD_B25_BUFFER dbuf;
	int count = 0;
	double t1 = 0 ,t2 = 0;
	double w1 = 0 , w2 = 0;


	sfd = -1;
	dfd = -1;
	b25 = NULL;
	bcas = NULL;

	if( 1 != init_ring_buffer(rbuf,RING_BUF_SIZE)) {
		goto LAST;
	}

	sfd = _open(src, _O_BINARY|_O_RDONLY|_O_SEQUENTIAL);
	if(sfd < 0){
		fprintf(stderr, "error - failed on _open(%s) [src]\n", src);
		goto LAST;
	}
#ifdef HAS_FADVISE
	if( -1 == posix_fadvise(sfd, 0, 0, POSIX_FADV_SEQUENTIAL) ){
		fprintf(stderr, "warn - posix_fadvise errno %d \n",errno);
	}
#endif

	_lseeki64(sfd, 0, SEEK_END);
	total = _telli64(sfd);
	_lseeki64(sfd, 0, SEEK_SET);

	b25 = create_arib_std_b25();
	if(b25 == NULL){
		fprintf(stderr, "error - failed on create_arib_std_b25()\n");
		goto LAST;
	}

	code = b25->set_multi2_round(b25, opt->round);
	if(code < 0){
		fprintf(stderr, "error - failed on ARIB_STD_B25::set_multi2_round() : code=%d\n", code);
		goto LAST;
	}

	code = b25->set_strip(b25, opt->strip);
	if(code < 0){
		fprintf(stderr, "error - failed on ARIB_STD_B25::set_strip() : code=%d\n", code);
		goto LAST;
	}

	code = b25->set_emm_proc(b25, opt->emm);
	if(code < 0){
		fprintf(stderr, "error - failed on ARIB_STD_B25::set_emm_proc() : code=%d\n", code);
		goto LAST;
	}

	bcas = create_b_cas_card();
	if(bcas == NULL){
		fprintf(stderr, "error - failed on create_b_cas_card()\n");
		goto LAST;
	}

	code = bcas->init(bcas);
	if(code < 0){
		fprintf(stderr, "error - failed on B_CAS_CARD::init() : code=%d\n", code);
		goto LAST;
	}

	code = b25->set_b_cas_card(b25, bcas);
	if(code < 0){
		fprintf(stderr, "error - failed on ARIB_STD_B25::set_b_cas_card() : code=%d\n", code);
		goto LAST;
	}

	dfd = _open(dst, _O_BINARY|_O_WRONLY|_O_SEQUENTIAL|_O_CREAT|_O_TRUNC, _S_IREAD|_S_IWRITE);
	if(dfd < 0){
		fprintf(stderr, "error - failed on _open(%s) [dst]\n", dst);
		goto LAST;
	}

	thread_args.sfd  = sfd;
	thread_args.b25 = b25;
	thread_args.rbuf = rbuf;

	if( 0 > pthread_create(&loader,NULL, loader_main, (void *)&thread_args) ) {
		printf("failed thread creation \n");
		goto LAST;
	}

	offset = 0;
	cbuf = &rbuf[0];
	while(1){

#if 0
		if( offset > 30000000 ){
			goto LAST;
		}
#endif 
		if(loader_status == -1) {
			pthread_join(loader, NULL);
			break;
		}
		if(cbuf->flag != 2 ){
			DEBUG("writer wait flag:%d used:%d\n",cbuf->flag, used);
			//pthread_cond_broadcast(&rcond);
			pthread_cond_wait(&wcond,&mut2);
			//pthread_cond_broadcast(&rcond);
			DEBUG("writer wake flag:%d used:%d\n",cbuf->flag, used);
			// cbuf = cbuf->next;
			continue;
		}

		sbuf.data = cbuf->data;
		sbuf.size = cbuf->size;

		cbuf->flag = 3;

		t2 = gettimeofday_sec();
		code = b25->put(b25, &sbuf);
		if(code < 0){
			fprintf(stderr, "error - failed on ARIB_STD_B25::put() : code=%d\n", code);
			goto LAST;
		}
		cbuf->flag = 0;
		used--;
		// futex(&used,FUTEX_WAKE,2,NULL);
		wake_thread(&rcond);
		code = b25->get(b25, &dbuf);
		t1 +=  (gettimeofday_sec() - t2);

		if(code < 0){
			fprintf(stderr, "error - failed on ARIB_STD_B25::get() : code=%d\n", code);
			goto LAST;
		}

		if(dbuf.size > 0){
			w2 = gettimeofday_sec();
			n = _write(dfd, dbuf.data, dbuf.size);
			w1 +=  (gettimeofday_sec() - w2);
			if(n != dbuf.size){
				fprintf(stderr, "error failed on _write(%d)\n", dbuf.size);
				goto LAST;
			}
		}
		count++;
		
		offset += sbuf.size;
		if(opt->verbose != 0){
			m = (int)(10000*offset/total);
			fprintf(stderr, "\rprocessing: %2d.%02d%% ", m/100, m%100);
		}
		cbuf = cbuf->next;
	}

	code = b25->flush(b25);
	if(code < 0){
		fprintf(stderr, "error - failed on ARIB_STD_B25::flush() : code=%d\n", code);
		goto LAST;
	}
	
	code = b25->get(b25, &dbuf);
	if(code < 0){
		fprintf(stderr, "error - failed on ARIB_STD_B25::get() : code=%d\n", code);
		goto LAST;
	}

	if(dbuf.size > 0){
		n = _write(dfd, dbuf.data, dbuf.size);
		if(n != dbuf.size){
			fprintf(stderr, "error - failed on _write(%d)\n", dbuf.size);
			goto LAST;
		}
	}

	if( offset > 10000 ){
		goto LAST;
	}

	if(opt->verbose != 0){
		fprintf(stderr, "\rprocessing: finish  \n");
		fflush(stderr);
		fflush(stdout);
	}

	n = b25->get_program_count(b25);
	if(n < 0){
		fprintf(stderr, "error - failed on ARIB_STD_B25::get_program_count() : code=%d\n", code);
		goto LAST;
	}
	for(i=0;i<n;i++){
		code = b25->get_program_info(b25, &pgrm, i);
		if(code < 0){
			fprintf(stderr, "error - failed on ARIB_STD_B25::get_program_info(%d) : code=%d\n", i, code);
			goto LAST;
		}
		if(pgrm.ecm_unpurchased_count > 0){
			fprintf(stderr, "warning - unpurchased ECM is detected\n");
			fprintf(stderr, "  channel:               %d\n", pgrm.program_number);
			fprintf(stderr, "  unpurchased ECM count: %d\n", pgrm.ecm_unpurchased_count);
			fprintf(stderr, "  last ECM error code:   %04x\n", pgrm.last_ecm_error_code);
			#if defined(WIN32)
			fprintf(stderr, "  undecrypted TS packet: %d\n", pgrm.undecrypted_packet_count);
			fprintf(stderr, "  total TS packet:       %d\n", pgrm.total_packet_count);
			#else
			fprintf(stderr, "  undecrypted TS packet: %"PRId64"\n", pgrm.undecrypted_packet_count);
			fprintf(stderr, "  total TS packet:       %"PRId64"\n", pgrm.total_packet_count);
			#endif
		}
	}

	if(opt->power_ctrl != 0){
		show_bcas_power_on_control_info(bcas);
	}

LAST:

	DEBUG("last\n");
	fprintf(stderr,"b25 time= = %10.30f \n",t1);
	fprintf(stderr,"read time= = %10.30f \n",r1);
	fprintf(stderr,"write time= = %10.30f \n",w1);
	fprintf(stderr,"count of decrypt: %d\n", count);
	if(sfd >= 0){
		_close(sfd);
		sfd = -1;
	}

	if(dfd >= 0){
		_close(dfd);
		dfd = -1;
	}

	if(b25 != NULL){
		b25->release(b25);
		b25 = NULL;
	}

	if(bcas != NULL){
		bcas->release(bcas);
		bcas = NULL;
	}
}

static void show_bcas_power_on_control_info(B_CAS_CARD *bcas)
{
	int code;
	int i,w;
	B_CAS_PWR_ON_CTRL_INFO pwc;

	code = bcas->get_pwr_on_ctrl(bcas, &pwc);
	if(code < 0){
		fprintf(stderr, "error - failed on B_CAS_CARD::get_pwr_on_ctrl() : code=%d\n", code);
		return;
	}

	if(pwc.count == 0){
		fprintf(stdout, "no EMM receiving request\n");
		return;
	}

	fprintf(stdout, "total %d EMM receiving request\n", pwc.count);
	for(i=0;i<pwc.count;i++){
		fprintf(stdout, "+ [%d] : tune ", i);
		switch(pwc.data[i].network_id){
		case 4:
			w = pwc.data[i].transport_id;
			fprintf(stdout, "BS-%d/TS-%d ", ((w >> 4) & 0x1f), (w & 7));
			break;
		case 6:
		case 7:
			w = pwc.data[i].transport_id;
			fprintf(stdout, "ND-%d/TS-%d ", ((w >> 4) & 0x1f), (w & 7));
			break;
		default:
			fprintf(stdout, "unknown(b:0x%02x,n:0x%04x,t:0x%04x) ", pwc.data[i].broadcaster_group_id, pwc.data[i].network_id, pwc.data[i].transport_id);
			break;
		}
		fprintf(stdout, "between %04d %02d/%02d ", pwc.data[i].s_yy, pwc.data[i].s_mm, pwc.data[i].s_dd);
		fprintf(stdout, "to %04d %02d/%02d ", pwc.data[i].l_yy, pwc.data[i].l_mm, pwc.data[i].l_dd);
		fprintf(stdout, "least %d hours\n", pwc.data[i].hold_time);
	}
}

