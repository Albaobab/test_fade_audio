#include <alsa/asoundlib.h>
#include <stdio.h>
#include <libavutil/mem.h>
#include <stddef.h>
#include <signal.h>
#include <ncurses.h>

#define FADE_DURATION_MS 2000
#define BLOCK_SAMPLING 512
#define BLOCK_SIZE 16
#define DEVICE "default"
#define SOUND_RATE 16000


static volatile sig_atomic_t stopAll = 0;
/**
 * Fonction permettant de bien fermer l'application quans le signal SIGINT est envoyé au processus parent
 * @param sig
 */
static void stop_handler(int sig) {
    if (sig == SIGINT) {
        stopAll = 1;
    }
}


static volatile sig_atomic_t stopChild = 0;
/**
 * Fonction permettant au processus parent de demander au processus fils de se stopper via un signal SIGCHLD
 * @param sig
 */
static void child_stop_handler(int sig) {
    if (sig == SIGCHLD) {
        stopChild = 1;
    }
}

/**
 * Fonction permettant d'initialiser l'utilisation du son
 * @param handle Transporte une structure contenant les informations du service audio
 * @return
 */
int init_sound(snd_pcm_t **handle) {
    int err;

    if ((err = snd_pcm_open(handle, DEVICE, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        return err;
    }

    if ((err = snd_pcm_set_params(*handle,
                                  SND_PCM_FORMAT_S16_LE,
                                  SND_PCM_ACCESS_RW_INTERLEAVED,
                                  1,
                                  SOUND_RATE,
                                  0,
                                  0)) < 0) {   /* 0.5sec */
        return err;
    }

    return 0;
}

/**
 * Fonction permettant d'arrêter l'utilisation du son.
 * @param handle Transporte une structure contenant les informations du service audio
 * @return
 */
int stop_sound(snd_pcm_t *handle) {
    int err;

    if ((err = snd_pcm_drain(handle)) < 0) {
        return err;
    }
    snd_pcm_close(handle);

    return 0;
}

/**
 * Fonction permettant d'initialiser le mix du son
 * @param elem Transporte une structure contenant les informaions des éléments audio
 * @param handle Transporte une structure contenant les informations du service audio
 * @return
 */
int init_volume_mixer(snd_mixer_elem_t** elem, snd_mixer_t **handle) {

    snd_mixer_selem_id_t *sid;
    const char *card = "hw:0";
    const char *selem_name = "Master";

    snd_mixer_open(handle,1);
    snd_mixer_attach(*handle, card);
    snd_mixer_selem_register(*handle, NULL, NULL);
    snd_mixer_load(*handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    *elem = snd_mixer_find_selem(*handle, sid);

    snd_mixer_selem_set_playback_volume(*elem, 1, 32655/2);
    return 0;
}

/**
 * Fonction permettant de terminer le mix du son
 * @param handle Transporte une structure contenant les informations du service audio
 * @return
 */
int stop_volume_mixer(snd_mixer_t *handle) {
    return snd_mixer_close(handle);
}

/**
 * Fonction permettant de faire augmenter au fur et à mesure le son
 * @param elem Transporte une structure contenant les informaions des éléments audio
 * @param base_volume Volume au début du programme
 * @param previous_volume Volume au tour précédent
 */
void volume_fader_up(snd_mixer_elem_t** elem, long base_volume, long *previous_volume) {
    if (*previous_volume < base_volume * 1.26) {
        *previous_volume += ((base_volume * 1.26) - *previous_volume) / 10; // 1.26 is the power needed to have 1.0dB gain
        snd_mixer_selem_set_playback_volume_all(*elem, *previous_volume);
        clear();
        printw("Appuyez sur 'f' pour appliquer du gain.\n[UP] Volume de base: %ld Volume actuel: %ld\n", base_volume, *previous_volume);
    }
}

/**
 * Fonction permettant de faire réduire au fur et à mesure le son
 * @param elem Transporte une structure contenant les informaions des éléments audio
 * @param base_volume Volume au début du programme
 * @param previous_volume Volume au tour précédent
 */
void volume_fader_down(snd_mixer_elem_t** elem, long base_volume, long *previous_volume) {

    if (*previous_volume > base_volume) {
        *previous_volume -= ((base_volume * 1.26) - *previous_volume) / 20; // 1.26 is the power needed to have 1.0dB gain
        snd_mixer_selem_set_playback_volume_all(*elem, *previous_volume);
        clear();
        printw("Appuyez sur 'f' pour appliquer du gain.\n[DOWN] Volume de base: %ld  Volume actuel: %ld\n", base_volume, *previous_volume);
    }
}

/***
 * Fonction pricipale du processus Fils, qui va s'occuper de l'interface homme-machine
 * @param elem Transporte une structure contenant les informaions des éléments audio
 * @return
 */
int child_main(snd_mixer_elem_t** elem) {
    initscr();
    noecho();
    nodelay(stdscr, 1);
    printw("Appuyez sur 'f' pour appliquer du gain.\n");

    long base_volume, current_volume;
    snd_mixer_selem_get_playback_volume(*elem, 1, &base_volume);
    printw("Volume actuel: %ld\n", base_volume);

    current_volume = base_volume;

    while (!stopChild) {
        if(!stopChild && getch() != 'f') {
            volume_fader_down(elem, base_volume, &current_volume);
        } else if (!stopChild) {
            volume_fader_up(elem, base_volume, &current_volume);
        }
        struct timespec time = {
            0,
            (FADE_DURATION_MS/20) * 1000000
        };
        nanosleep(&time, NULL);

    }
    endwin();

    return EXIT_SUCCESS;
}

/**
 * Fonction principale du processus parent, qui va s'occuper de lire le fichier et le mettre sur la sorte audio
 * @param handle Transporte une structure contenant les informations du service audio
 * @param file_name Nom du fichier d'entrée
 * @return
 */
int parent_main(snd_pcm_t** handle, char* file_name) {

    int err = 0;

    snd_pcm_sframes_t frames;
    size_t buff_size = BLOCK_SAMPLING*BLOCK_SIZE;
    void *buff = malloc(buff_size);


    FILE *fp = fopen(file_name, "r");

    fseek(fp, 0, SEEK_END);
    long end = ftell(fp);
    fseek(fp, buff_size, SEEK_SET);

    while(!stopAll && ftell(fp) < end) {

        if ((err = fread(buff, BLOCK_SIZE, BLOCK_SAMPLING, fp)) == 0) {
            printf("Fin de fichier inattendue.\n");
            break;
        }

        frames = snd_pcm_writei(*handle, buff, buff_size/2);
        if (frames < 0)
            frames = snd_pcm_recover(*handle, frames, 0);
        if (frames < 0) {
            printf("Erreur de lecture: %s\n", snd_strerror(frames));
            break;
        }
    }

    free(buff);
    fclose(fp);

    return err;
}


/**
 * Fonction principale
 * @param argc Nombre d'arguments
 * @param argv Arguments
 * @return
 */
int main(int argc, char ** argv){
    signal(SIGINT, stop_handler);
    signal(SIGCHLD, child_stop_handler);

    int err = EXIT_SUCCESS;

    if(argc < 2) {
        fprintf(stderr, "Veuillez s'il vous plait renseigner un fichier audio à lire.\nPour rappel, le programme fonctionne comme tel: %s <filename>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    snd_pcm_t *pcm_handle;

    if ((err = init_sound(&pcm_handle)) < 0) {
        fprintf(stderr,"Erreur à l'initialisation du son: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    snd_mixer_t *mixer_handle;
    snd_mixer_elem_t* elem;

    if ((err = init_volume_mixer(&elem, &mixer_handle)) < 0) {
        fprintf(stderr,"Erreur à l'initialisation du selecteur de volume: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    // le processus parent produira le son, le processus fils va gérer l'interface homme-machine
    pid_t pid = fork();
    if (pid == 0) {
        child_main(&elem);
    } else if (pid >= 0) {
        parent_main(&pcm_handle, argv[1]);
        kill(0, SIGCHLD);

        if ((err = stop_sound(pcm_handle)) < 0) {
            fprintf(stderr, "Erreur pendant l'extinction du son: %s\n", snd_strerror(err));
        }

        if ((err = stop_volume_mixer(mixer_handle)) < 0) {
            fprintf(stderr, "Erreur pendant l'extinction du selecteur de volume: %s\n", snd_strerror(err));
        }

    } else {
        fprintf(stderr, "Impossible de créer le processus fils\n");
    }

    return err;
}
